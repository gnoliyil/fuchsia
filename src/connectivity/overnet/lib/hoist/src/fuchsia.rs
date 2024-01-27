// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![cfg(target_os = "fuchsia")]

use {
    anyhow::{Context as _, Error},
    fidl_fuchsia_overnet::{
        MeshControllerMarker, MeshControllerProxy, ServiceConsumerMarker, ServiceConsumerProxy,
        ServicePublisherMarker, ServicePublisherProxy,
    },
    fuchsia_component,
};

#[derive(Debug, Clone)]
pub struct Hoist;

impl Hoist {
    pub(crate) fn new() -> Result<Self, Error> {
        Ok(Self)
    }

    /// Performs initial configuration with appropriate defaults for the implementation and platform.
    ///
    /// On a fuchsia device this will likely do nothing, so that is the default implementation.
    /// On a host platform it will use the environment variable ASCENDD to find the socket, or
    /// use a default address.
    pub fn start_default_link(&self, _enable_circuit: crate::Cso) -> Result<(), Error> {
        Ok(())
    }
}

impl super::OvernetInstance for Hoist {
    fn connect_as_service_consumer(&self) -> Result<ServiceConsumerProxy, Error> {
        Ok(fuchsia_component::client::connect_to_protocol::<ServiceConsumerMarker>()
            .context("Failed to connect to overnet ServiceConsumer service")?)
    }

    fn connect_as_service_publisher(&self) -> Result<ServicePublisherProxy, Error> {
        Ok(fuchsia_component::client::connect_to_protocol::<ServicePublisherMarker>()
            .context("Failed to connect to overnet ServicePublisher service")?)
    }

    fn connect_as_mesh_controller(&self) -> Result<MeshControllerProxy, Error> {
        Ok(fuchsia_component::client::connect_to_protocol::<MeshControllerMarker>()
            .context("Failed to connect to overnet MeshController service")?)
    }
}
