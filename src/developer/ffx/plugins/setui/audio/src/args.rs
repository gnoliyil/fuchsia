// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use argh::FromArgs;
use ffx_core::ffx_command;
use fidl_fuchsia_settings::{AudioSettings, AudioStreamSettingSource, AudioStreamSettings, Volume};

#[ffx_command()]
#[derive(FromArgs, Debug, PartialEq, Clone, Copy)]
#[argh(subcommand, name = "audio")]
/// get or set audio settings
pub struct Audio {
    // AudioStreams
    /// which stream should be modified. Valid options are background, media, interruption,
    /// system_agent, and communication
    #[argh(option, short = 't', from_str_fn(str_to_audio_stream))]
    pub stream: Option<fidl_fuchsia_media::AudioRenderUsage>,

    /// which source is changing the stream. Valid options are user, system, and
    /// system_with_feedback
    #[argh(option, short = 's', from_str_fn(str_to_audio_source))]
    pub source: Option<AudioStreamSettingSource>,

    // UserVolume
    /// the volume level specified as a float in the range [0, 1]
    #[argh(option, short = 'l')]
    pub level: Option<f32>,

    /// whether or not the volume is muted
    #[argh(option, short = 'v')]
    pub volume_muted: Option<bool>,
}

fn str_to_audio_stream(src: &str) -> Result<fidl_fuchsia_media::AudioRenderUsage, String> {
    match src.to_lowercase().as_str() {
        "background" | "b" => Ok(fidl_fuchsia_media::AudioRenderUsage::Background),
        "media" | "m" => Ok(fidl_fuchsia_media::AudioRenderUsage::Media),
        "interruption" | "i" => Ok(fidl_fuchsia_media::AudioRenderUsage::Interruption),
        "system_agent" | "systemagent" | "system agent" | "s" => {
            Ok(fidl_fuchsia_media::AudioRenderUsage::SystemAgent)
        }
        "communication" | "c" => Ok(fidl_fuchsia_media::AudioRenderUsage::Communication),
        _ => Err(String::from("Couldn't parse audio stream type")),
    }
}

fn str_to_audio_source(
    src: &str,
) -> Result<fidl_fuchsia_settings::AudioStreamSettingSource, String> {
    match src.to_lowercase().as_str() {
        "user" | "u" => Ok(fidl_fuchsia_settings::AudioStreamSettingSource::User),
        "system" | "s" => Ok(fidl_fuchsia_settings::AudioStreamSettingSource::System),
        "system_with_feedback" | "f" => {
            Ok(fidl_fuchsia_settings::AudioStreamSettingSource::SystemWithFeedback)
        }
        _ => Err(String::from("Couldn't parse audio source type")),
    }
}

impl TryFrom<Audio> for AudioSettings {
    type Error = &'static str;

    fn try_from(src: Audio) -> Result<Self, Self::Error> {
        let volume = Volume { level: src.level, muted: src.volume_muted, ..Default::default() };
        let stream_settings = AudioStreamSettings {
            stream: src.stream,
            source: src.source,
            user_volume: if volume == Volume::default() { None } else { Some(volume) },
            ..Default::default()
        };

        let result = AudioSettings {
            streams: if stream_settings == AudioStreamSettings::default() {
                None
            } else {
                Some(vec![stream_settings])
            },
            ..Default::default()
        };

        // TODO(https://fxbug.dev/52556): Clean up this logic once we have a detailed error return
        // from the FIDL.
        if result == AudioSettings::default() {
            // A Watch call request.
            return Ok(result);
        } else if src.stream.is_none() {
            return Err("Missing stream type.");
        } else if src.source.is_none() {
            return Err("Missing stream source.");
        } else if src.level.is_none() && src.volume_muted.is_none() {
            return Err("User volume level and volume muted cannot be None at the same time.");
        }
        // A valid Set call request.
        Ok(result)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    const CMD_NAME: &'static [&'static str] = &["audio"];

    #[test]
    fn test_audio_cmd() {
        // Test input arguments are generated to according struct.
        let render = "background";
        let source = "system";
        let level = "0.6";
        let not_muted = "false";
        let args = &["-t", render, "-s", source, "-l", level, "-v", not_muted];
        assert_eq!(
            Audio::from_args(CMD_NAME, args),
            Ok(Audio {
                stream: Some(str_to_audio_stream(render).unwrap()),
                source: Some(str_to_audio_source(source).unwrap()),
                level: Some(0.6),
                volume_muted: Some(false),
            })
        )
    }
}
