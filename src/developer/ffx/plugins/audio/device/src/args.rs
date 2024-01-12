// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::Result,
    argh::{ArgsInfo, FromArgs},
    ffx_core::ffx_command,
    format_utils::Format,
    std::str::FromStr,
};

#[ffx_command()]
#[derive(ArgsInfo, FromArgs, Debug, PartialEq)]
#[argh(
    subcommand,
    name = "device",
    description = "Interact directly with device hardware.",
    example = "\
    To print information about a specific device: \n\
    \t$ ffx audio device --id 3d99d780 --direction input info \n\n\
    Play a wav file directly to device hardware: \n\
    \t$ cat ~/sine.wav | ffx audio device --id  a70075f2 play \n\n
    \t$ ffx audio device --id  a70075f2 play --file ~/sine.wav \n\n
    Record a wav file directly from device hardware: \n\
    \t$ ffx audio device --id 3d99d780 record --format 48000,uint8,1ch --duration 1s \n\n
    Mute the stream of the output device: \n
    \t$ ffx audio device --id a70075f2 --direction output mute \n\n
    Set the gain of the output device to -20 dB: \n
    \t$ ffx audio device --id a70075f2 --direction output gain -20 \n\n
    Turn agc on for the input device: \n
    \t$ ffx audio device --id 3d99d780 --direction input agc on"
)]
pub struct DeviceCommand {
    #[argh(subcommand)]
    pub subcommand: SubCommand,

    #[argh(
        option,
        description = "device id. stream device node id from /dev/class/audio-input/,\
        /dev/class/audio-output/, or /dev/class/audio-composite/.
        If not specified, command will default to first device alphabetically listed."
    )]
    pub id: Option<String>,

    #[argh(
        option,
        long = "direction",
        description = "device direction. Accepted values: input, output. \
        Play and record will use output and input respectively by default."
    )]
    pub device_direction: Option<DeviceDirection>,

    #[argh(
        option,
        long = "type",
        description = "device type. Accepted values: StreamConfig, Composite. \
        If not specified, defaults to StreamConfig",
        from_str_fn(parse_device_type),
        default = "fidl_fuchsia_hardware_audio::DeviceType::StreamConfig"
    )]
    pub device_type: fidl_fuchsia_hardware_audio::DeviceType,
}

#[derive(ArgsInfo, FromArgs, Debug, PartialEq)]
#[argh(subcommand)]
pub enum SubCommand {
    Info(InfoCommand),
    Play(DevicePlayCommand),
    Record(DeviceRecordCommand),
    Gain(DeviceGainCommand),
    Mute(DeviceMuteCommand),
    Unmute(DeviceUnmuteCommand),
    Agc(DeviceAgcCommand),
}

#[derive(ArgsInfo, FromArgs, Debug, PartialEq)]
#[argh(
    subcommand,
    name = "info",
    description = "List information about a specific audio device.",
    example = "ffx audio device --type StreamConfig --direction input info"
)]
pub struct InfoCommand {}

#[derive(ArgsInfo, FromArgs, Debug, PartialEq)]
#[argh(subcommand, name = "play", description = "Send audio data directly to device ring buffer.")]
pub struct DevicePlayCommand {
    #[argh(
        option,
        description = "file in WAV format containing audio signal. If not specified, \
        ffx command will read from stdin."
    )]
    pub file: Option<String>,
}

#[derive(Debug, PartialEq)]
pub enum InfoOutputFormat {
    Json,
    Text,
}

#[derive(ArgsInfo, FromArgs, Debug, PartialEq)]
#[argh(subcommand, name = "record", description = "Capture audio data directly from ring buffer.")]
pub struct DeviceRecordCommand {
    #[argh(
        option,
        description = "duration of output signal. Examples: 5ms or 3s. If not specified, \
        press ENTER to stop recording.",
        from_str_fn(parse_duration)
    )]
    pub duration: Option<std::time::Duration>,

    #[argh(option, description = "output format (see 'ffx audio help' for more information).")]
    pub format: Format,
}

#[derive(ArgsInfo, FromArgs, Debug, PartialEq)]
#[argh(
    subcommand,
    name = "gain",
    description = "Request to set the gain of the stream in decibels."
)]
pub struct DeviceGainCommand {
    #[argh(option, description = "gain (in decibles) to set the stream to.")]
    pub gain: f32,
}

#[derive(ArgsInfo, FromArgs, Debug, PartialEq)]
#[argh(subcommand, name = "mute", description = "Request to mute a stream.")]
pub struct DeviceMuteCommand {}

#[derive(ArgsInfo, FromArgs, Debug, PartialEq)]
#[argh(subcommand, name = "unmute", description = "Request to unmute a stream.")]
pub struct DeviceUnmuteCommand {}

#[derive(ArgsInfo, FromArgs, Debug, PartialEq)]
#[argh(
    subcommand,
    name = "agc",
    description = "Request to enable or disable automatic gain control for the stream."
)]
pub struct DeviceAgcCommand {
    #[argh(
        positional,
        description = "enable or disable agc. Accepted values: on, off",
        from_str_fn(string_to_enable)
    )]
    pub enable: bool,
}

fn string_to_enable(value: &str) -> Result<bool, String> {
    if value == "on" {
        Ok(true)
    } else if value == "off" {
        Ok(false)
    } else {
        Err(format!("Expected one of: on, off"))
    }
}

impl FromStr for InfoOutputFormat {
    type Err = anyhow::Error;
    fn from_str(s: &str) -> Result<Self, anyhow::Error> {
        match s.to_lowercase().as_str() {
            "json" => Ok(InfoOutputFormat::Json),
            "text" => Ok(InfoOutputFormat::Text),
            _ => {
                Err(anyhow::anyhow!("invalid format argument, {}. Expected one of: JSON, text", s))
            }
        }
    }
}

#[derive(Debug, PartialEq)]
pub enum DeviceDirection {
    Input,
    Output,
}

impl FromStr for DeviceDirection {
    type Err = anyhow::Error;
    fn from_str(s: &str) -> Result<Self, anyhow::Error> {
        match s.to_lowercase().as_str() {
            "input" => Ok(DeviceDirection::Input),
            "output" => Ok(DeviceDirection::Output),
            _ => Err(anyhow::anyhow!(
                "invalid device direction, {}. Expected one of: input, output",
                s
            )),
        }
    }
}

fn parse_device_type(value: &str) -> Result<fidl_fuchsia_hardware_audio::DeviceType, String> {
    match value.to_lowercase().as_str() {
        "composite" => Ok(fidl_fuchsia_hardware_audio::DeviceType::Composite),
        "streamconfig" => Ok(fidl_fuchsia_hardware_audio::DeviceType::StreamConfig),
        _ => {
            Err(format!("invalid device type, {}. Expected one of: Composite, StreamConfig", value))
        }
    }
}

fn parse_duration(value: &str) -> Result<std::time::Duration, String> {
    format_utils::parse_duration(value)
}
