// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    failure::Error,
    fidl_fuchsia_media::AudioRenderUsage,
    fidl_fuchsia_settings::{
        AudioInput, AudioProxy, AudioSettings, AudioStreamSettingSource, AudioStreamSettings,
        Volume,
    },
};

pub async fn command(
    proxy: AudioProxy,
    stream: Option<fidl_fuchsia_media::AudioRenderUsage>,
    source: Option<fidl_fuchsia_settings::AudioStreamSettingSource>,
    level: Option<f32>,
    volume_muted: Option<bool>,
    input_muted: Option<bool>,
) -> Result<String, Error> {
    let mut output = String::new();
    let mut audio_settings = AudioSettings::empty();
    let mut stream_settings = AudioStreamSettings::empty();
    let mut volume = Volume::empty();
    let mut input = AudioInput::empty();

    volume.level = level;
    volume.muted = volume_muted;
    stream_settings.stream = stream;
    stream_settings.source = source;
    stream_settings.user_volume = if volume == Volume::empty() { None } else { Some(volume) };
    input.muted = input_muted;
    audio_settings.streams = Some(vec![stream_settings]);
    audio_settings.input = if input == AudioInput::empty() { None } else { Some(input) };

    if audio_settings == AudioSettings::empty() {
        let settings = proxy.watch().await?;
        match settings {
            Ok(setting_value) => {
                let setting_string = format!("{:?}", setting_value);
                output.push_str(&setting_string);
            }
            Err(err) => output.push_str(&format!("{:?}", err)),
        }
    } else {
        let mutate_result = proxy.set(audio_settings).await?;
        match mutate_result {
            Ok(_) => {
                if let Some(stream_val) = stream {
                    output.push_str(&format!(
                        "Successfully set stream to {:?}",
                        describe_audio_stream(stream_val),
                    ));
                }
                if let Some(source_val) = source {
                    output.push_str(&format!(
                        "Successfully set source to {}",
                        describe_audio_source(source_val)
                    ));
                }
                if let Some(level_val) = level {
                    output.push_str(&format!("Successfully set level to {}", level_val));
                }
                if let Some(volume_muted_val) = volume_muted {
                    output.push_str(&format!(
                        "Successfully set volume_muted to {}",
                        volume_muted_val
                    ));
                }
                if let Some(input_muted_val) = input_muted {
                    output
                        .push_str(&format!("Successfully set input_muted to {}", input_muted_val));
                }
            }
            Err(err) => output.push_str(&format!("{:?}", err)),
        }
    }
    Ok(output)
}

fn describe_audio_stream(stream: AudioRenderUsage) -> String {
    match stream {
        AudioRenderUsage::Background => "Background",
        AudioRenderUsage::Media => "Media",
        AudioRenderUsage::SystemAgent => "System Agent",
        AudioRenderUsage::Communication => "Communication",
        _ => "UNKNOWN AUDIO ENUM VALUE",
    }
    .to_string()
}

fn describe_audio_source(source: AudioStreamSettingSource) -> String {
    match source {
        AudioStreamSettingSource::Default => "Default",
        AudioStreamSettingSource::User => "User",
        AudioStreamSettingSource::System => "System",
    }
    .to_string()
}
