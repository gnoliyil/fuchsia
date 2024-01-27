// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

///! This function demonstrates setting a single config with a static full-screen fill color and
///! sampling vsync events.
use {
    anyhow::{format_err, Context, Result},
    display_utils::{
        Controller, DisplayConfig, DisplayInfo, Image, ImageId, Layer, LayerConfig, PixelFormat,
        VsyncEvent,
    },
    futures::StreamExt,
    std::io::Write,
};

use crate::{draw::MappedImage, fps::Counter, rgb::Rgb888};

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
    // The display driver does not send vsync events to a client unless it successfully applies a
    // display configuration. Build a single full screen layer with a solid color to generate
    // hardware vsync events.
    // NOTE: An empty config results in artificial vsync events to be generated (notably in the
    // Intel driver). The config must contain at least one primary layer to obtain an accurate
    // hardware sample.

    let Args { display, color, pixel_format } = args;

    // Obtain the display resolution based on the display's preferred mode.
    let (width, height) = {
        let mode = display.0.modes[0];
        (mode.horizontal_resolution, mode.vertical_resolution)
    };
    let params = display_utils::ImageParameters {
        width,
        height,
        pixel_format,
        color_space: fidl_fuchsia_sysmem::ColorSpaceType::Srgb,
        name: Some("display-tool vsync layer".to_string()),
    };
    let image = MappedImage::create(Image::create(controller.clone(), ImageId(1), &params).await?)?;
    let bytes_to_fill = get_bytes_for_rgb_color(color, pixel_format)?;
    image.fill(&bytes_to_fill).context("failed to draw fill color")?;

    // Ensure that vsync events are enabled before we issue the first call to ApplyConfig.
    let mut vsync = controller.add_vsync_listener(Some(display.id()))?;

    let layer = controller.create_layer().await?;
    let configs = vec![DisplayConfig {
        id: display.id(),
        layers: vec![Layer {
            id: layer,
            config: LayerConfig::Primary {
                image_id: image.id(),
                image_config: params.into(),
                unblock_event: None,
                retirement_event: None,
            },
        }],
    }];
    controller.apply_config(&configs).await?;

    // Start sampling vsync frequency.
    let mut counter = Counter::new();
    while let Some(VsyncEvent { id, timestamp, .. }) = vsync.next().await {
        counter.add(timestamp);
        let stats = counter.stats();

        print!(
            "{}Display {} refresh rate {:.2} Hz ({:.5} ms)",
            CLEAR, id.0, stats.sample_rate_hz, stats.sample_time_delta_ms
        );
        std::io::stdout().flush()?;
    }

    Err(format_err!("stopped receiving vsync events"))
}
