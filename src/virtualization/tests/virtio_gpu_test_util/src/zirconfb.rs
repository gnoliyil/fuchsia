// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::framebuffer::{DetectResult, DisplayInfo, Framebuffer},
    anyhow::Error,
    fidl::endpoints,
    fidl_fuchsia_hardware_display::{
        CoordinatorEvent, CoordinatorMarker, CoordinatorSynchronousProxy, Info,
        ProviderSynchronousProxy,
    },
    fuchsia_zircon as zx,
    serde_json::json,
};

const DEVICE_PATH: &'static str = "/dev/class/display-controller/000";

fn convert_info(info: &Info) -> DisplayInfo {
    DisplayInfo {
        id: format!("[mfgr: '{}', model: '{}']", info.manufacturer_name, info.monitor_name),
        width: info.modes[0].horizontal_resolution,
        height: info.modes[0].vertical_resolution,
    }
}

fn read_info() -> Result<DetectResult, Error> {
    // Connect to the display coordinator.
    let provider = {
        let (client_end, server_end) = zx::Channel::create();
        fuchsia_component::client::connect_channel_to_protocol_at_path(server_end, DEVICE_PATH)?;
        ProviderSynchronousProxy::new(client_end)
    };
    let coordinator = {
        let (dc_client, dc_server) = endpoints::create_endpoints::<CoordinatorMarker>();
        provider.open_coordinator_for_primary(dc_server, zx::Time::INFINITE)?;
        CoordinatorSynchronousProxy::new(dc_client.into_channel())
    };

    // Wait for the 'OnDisplaysChanged' event.
    let displays = loop {
        match coordinator.wait_for_event(zx::Time::INFINITE)? {
            CoordinatorEvent::OnDisplaysChanged { added, .. } => break added,
            _ => {}
        }
    };
    Ok(DetectResult {
        displays: displays.iter().map(convert_info).collect(),
        details: json!(format!("{:#?}", displays)),
        ..Default::default()
    })
}

fn read_info_from_display_coordinator() -> DetectResult {
    read_info().unwrap_or_else(DetectResult::from_error)
}

pub struct ZirconFramebuffer;

impl Framebuffer for ZirconFramebuffer {
    fn detect_displays(&self) -> DetectResult {
        read_info_from_display_coordinator()
    }
}
