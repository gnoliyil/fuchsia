// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

///! This function demonstrates setting a single config with a static full-screen fill color and
///! sampling vsync events.
use {
    anyhow::{format_err, Result},
    display_utils::{
        Controller, DisplayConfig, DisplayInfo, Layer, LayerConfig, PixelFormat, VsyncEvent,
    },
    futures::StreamExt,
    std::io::Write,
};

use crate::{fps::Counter, rgb::Rgb888};

const CLEAR: &str = "\x1B[2K\r";

pub struct Args<'a> {
    pub display: &'a DisplayInfo,
    pub color: Rgb888,
    pub pixel_format: PixelFormat,
}

pub fn get_bytes_for_rgb_color(rgb: Rgb888, pixel_format: PixelFormat) -> Result<Vec<u8>> {
    match pixel_format {
        PixelFormat::Argb8888 | PixelFormat::RgbX888 => {
            Ok(vec![rgb.b, rgb.g, rgb.r, /*alpha=*/ 255])
        }
        PixelFormat::Abgr8888 | PixelFormat::Bgr888X => {
            Ok(vec![rgb.r, rgb.g, rgb.b, /*alpha=*/ 255])
        }
        _ => Err(anyhow::format_err!("unsupported pixel format {}", pixel_format)),
    }
}

pub async fn run<'a>(controller: &Controller, args: Args<'a>) -> Result<()> {
    let Args { display, color, pixel_format } = args;
    let color_bytes = get_bytes_for_rgb_color(color, pixel_format)?;

    // Ensure that vsync events are enabled before we issue the first call to ApplyConfig.
    let mut vsync = controller.add_vsync_listener(Some(display.id()))?;

    let layer = controller.create_layer().await?;
    let configs = vec![DisplayConfig {
        id: display.id(),
        layers: vec![Layer { id: layer, config: LayerConfig::Color { pixel_format, color_bytes } }],
    }];
    controller.apply_config(&configs).await?;
    let recent_applied_config_stamp = controller.get_recent_applied_config_stamp().await?;

    // The color layer should be displayed on the screen and Vsync events
    // should start.
    let mut counter = Counter::new();
    let mut config_applied = false;
    while let Some(VsyncEvent { id, timestamp, config }) = vsync.next().await {
        counter.add(timestamp);
        let stats = counter.stats();
        config_applied |= config.value == recent_applied_config_stamp;

        print!(
            "{}Display {} config {} applied, refresh rate {:.2} Hz ({:.5} ms)",
            CLEAR,
            id.0,
            if config_applied { "is" } else { "is not" },
            stats.sample_rate_hz,
            stats.sample_time_delta_ms
        );
        std::io::stdout().flush()?;
    }

    Err(format_err!("stopped receiving vsync events"))
}
