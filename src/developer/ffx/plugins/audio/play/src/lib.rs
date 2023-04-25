// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::Result,
    async_trait::async_trait,
    ffx_audio_play_args::{
        AudioRenderUsageExtended::{
            Background, Communication, Interruption, Media, SystemAgent, Ultrasound,
        },
        PlayCommand,
    },
    fho::{selector, FfxMain, FfxTool, SimpleWriter},
    fidl_fuchsia_audio_ffxdaemon::{AudioDaemonPlayRequest, AudioDaemonProxy},
};

#[derive(FfxTool)]
pub struct PlayTool {
    #[command]
    cmd: PlayCommand,
    #[with(selector("core/audio_ffx_daemon:expose:fuchsia.audio.ffxdaemon.AudioDaemon"))]
    audio_proxy: AudioDaemonProxy,
}

fho::embedded_plugin!(PlayTool);
#[async_trait(?Send)]
impl FfxMain for PlayTool {
    type Writer = SimpleWriter;
    async fn main(self, _writer: Self::Writer) -> fho::Result<()> {
        let (play_remote, play_local) = fidl::Socket::create_datagram();

        let renderer = match self.cmd.usage {
            Ultrasound => fidl_fuchsia_audio_ffxdaemon::RendererType::UltrasoundRenderer(
                fidl_fuchsia_audio_ffxdaemon::UltrasoundRenderer {
                    packet_count: self.cmd.packet_count,
                    ..fidl_fuchsia_audio_ffxdaemon::UltrasoundRenderer::EMPTY
                },
            ),

            Background(usage) | Media(usage) | SystemAgent(usage) | Communication(usage)
            | Interruption(usage) => fidl_fuchsia_audio_ffxdaemon::RendererType::StandardRenderer(
                fidl_fuchsia_audio_ffxdaemon::RendererConfig {
                    usage: Some(usage),
                    clock: Some(self.cmd.clock),
                    ..fidl_fuchsia_audio_ffxdaemon::RendererConfig::EMPTY
                },
            ),
        };

        let request = AudioDaemonPlayRequest {
            socket: Some(play_remote),
            location: Some(fidl_fuchsia_audio_ffxdaemon::PlayLocation::Renderer(renderer)),
            gain_settings: Some(fidl_fuchsia_audio_ffxdaemon::GainSettings {
                mute: Some(self.cmd.mute),
                gain: Some(self.cmd.gain),
                ..fidl_fuchsia_audio_ffxdaemon::GainSettings::EMPTY
            }),
            ..AudioDaemonPlayRequest::EMPTY
        };

        ffx_audio_common::play(request, self.audio_proxy, play_local).await.map_err(Into::into)
    }
}
