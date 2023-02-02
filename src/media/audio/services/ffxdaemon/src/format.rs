// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{Error, Result},
    fidl_fuchsia_media::AudioSampleFormat,
    regex::Regex,
    std::{convert::From, str::FromStr, time::Duration},
};

pub const DURATION_REGEX: &'static str = r"^(\d+)(h|m|s|ms)$";

#[derive(Debug, Eq, PartialEq, Clone)]
pub struct Format {
    pub sample_type: fidl_fuchsia_media::AudioSampleFormat,
    pub frames_per_second: u32,
    pub channels: u32,
}

impl Format {
    pub fn bytes_per_frame(&self) -> u32 {
        match self.sample_type {
            fidl_fuchsia_media::AudioSampleFormat::Unsigned8 => 1,
            fidl_fuchsia_media::AudioSampleFormat::Signed16 => 2,
            fidl_fuchsia_media::AudioSampleFormat::Signed24In32 => 4,
            fidl_fuchsia_media::AudioSampleFormat::Float => 4,
        }
    }

    pub fn bytes_per_sample(&self) -> u32 {
        return self.bytes_per_frame() * self.channels;
    }

    pub fn frames_in_duration(&self, duration: std::time::Duration) -> u64 {
        (self.frames_per_second as f64 * duration.as_secs_f64()).ceil() as u64
    }

    pub fn valid_bits_per_sample(&self) -> u32 {
        match self.sample_type {
            fidl_fuchsia_media::AudioSampleFormat::Unsigned8 => 8,
            fidl_fuchsia_media::AudioSampleFormat::Signed16 => 16,
            fidl_fuchsia_media::AudioSampleFormat::Signed24In32 => 24,
            fidl_fuchsia_media::AudioSampleFormat::Float => 32,
        }
    }

    pub fn silence_value(&self) -> u8 {
        match self.sample_type {
            fidl_fuchsia_media::AudioSampleFormat::Unsigned8 => 128,
            _ => 0,
        }
    }

    pub fn is_supported_by(
        &self,
        supported_formats: Vec<fidl_fuchsia_hardware_audio::SupportedFormats>,
    ) -> bool {
        let hardware_format = fidl_fuchsia_hardware_audio::Format::from(self);
        let mut is_format_supported = false;
        for supported_format in supported_formats {
            let pcm_formats = supported_format.pcm_supported_formats.unwrap();

            if pcm_formats.frame_rates.unwrap().contains(&self.frames_per_second)
                && pcm_formats.bytes_per_sample.unwrap().contains(&(self.bytes_per_sample() as u8))
                && pcm_formats
                    .sample_formats
                    .unwrap()
                    .contains(&hardware_format.pcm_format.unwrap().sample_format)
                && pcm_formats
                    .valid_bits_per_sample
                    .unwrap()
                    .contains(&(self.valid_bits_per_sample() as u8))
                && pcm_formats.channel_sets.unwrap().into_iter().any(|channel_set| {
                    channel_set.attributes.unwrap().len() == self.channels as usize
                })
            {
                is_format_supported = true;
                break;
            }
        }
        is_format_supported
    }
}

impl From<&hound::WavSpec> for Format {
    fn from(item: &hound::WavSpec) -> Self {
        Format {
            sample_type: match item.sample_format {
                hound::SampleFormat::Int => match item.bits_per_sample {
                    0..=8 => fidl_fuchsia_media::AudioSampleFormat::Unsigned8,
                    9..=16 => fidl_fuchsia_media::AudioSampleFormat::Signed16,
                    17.. => fidl_fuchsia_media::AudioSampleFormat::Signed24In32,
                },
                hound::SampleFormat::Float => fidl_fuchsia_media::AudioSampleFormat::Float,
            },
            frames_per_second: item.sample_rate,
            channels: item.channels as u32,
        }
    }
}

impl From<&fidl_fuchsia_media::AudioStreamType> for Format {
    fn from(item: &fidl_fuchsia_media::AudioStreamType) -> Self {
        Format {
            sample_type: item.sample_format,
            frames_per_second: item.frames_per_second,
            channels: item.channels,
        }
    }
}

impl From<&Format> for fidl_fuchsia_hardware_audio::Format {
    fn from(item: &Format) -> fidl_fuchsia_hardware_audio::Format {
        fidl_fuchsia_hardware_audio::Format {
            pcm_format: Some(fidl_fuchsia_hardware_audio::PcmFormat {
                number_of_channels: item.channels as u8,
                sample_format: match item.sample_type {
                    AudioSampleFormat::Unsigned8 => {
                        fidl_fuchsia_hardware_audio::SampleFormat::PcmUnsigned
                    }
                    AudioSampleFormat::Signed16 => {
                        fidl_fuchsia_hardware_audio::SampleFormat::PcmSigned
                    }
                    AudioSampleFormat::Signed24In32 => {
                        fidl_fuchsia_hardware_audio::SampleFormat::PcmSigned
                    }
                    AudioSampleFormat::Float => fidl_fuchsia_hardware_audio::SampleFormat::PcmFloat,
                },
                bytes_per_sample: item.bytes_per_sample() as u8,
                valid_bits_per_sample: item.valid_bits_per_sample() as u8,
                frame_rate: item.frames_per_second,
            }),
            ..fidl_fuchsia_hardware_audio::Format::EMPTY
        }
    }
}

impl From<&Format> for hound::WavSpec {
    fn from(item: &Format) -> Self {
        hound::WavSpec {
            channels: item.channels as u16,
            sample_format: match item.sample_type {
                fidl_fuchsia_media::AudioSampleFormat::Float => hound::SampleFormat::Float,
                _ => hound::SampleFormat::Int,
            },
            sample_rate: item.frames_per_second,
            bits_per_sample: match item.sample_type {
                fidl_fuchsia_media::AudioSampleFormat::Unsigned8 => 8,
                fidl_fuchsia_media::AudioSampleFormat::Signed16 => 16,
                fidl_fuchsia_media::AudioSampleFormat::Signed24In32 => 32,
                fidl_fuchsia_media::AudioSampleFormat::Float => 32,
            },
        }
    }
}

impl From<&Format> for fidl_fuchsia_media::AudioStreamType {
    fn from(item: &Format) -> Self {
        fidl_fuchsia_media::AudioStreamType {
            sample_format: item.sample_type,
            channels: item.channels,
            frames_per_second: item.frames_per_second,
        }
    }
}

impl FromStr for Format {
    type Err = Error;

    fn from_str(s: &str) -> Result<Self, Error> {
        if s.len() == 0 {
            return Err(anyhow::anyhow!("No format specified."));
        };

        let splits: Vec<&str> = s.split(",").collect();

        if splits.len() != 3 {
            return Err(anyhow::anyhow!(
                "Expected 3 comma-separated values: 
            <SampleRate>,<SampleType>,<Channels> but have {}.",
                splits.len()
            ));
        }

        let frame_rate = match splits[0].parse::<u32>() {
            Ok(sample_rate) => Ok(sample_rate),
            Err(_) => Err(anyhow::anyhow!("First value (sample rate) should be an integer.")),
        }?;

        let sample_type = match CommandSampleType::from_str(splits[1]) {
            Ok(sample_type) => Ok(sample_type),
            Err(_) => Err(anyhow::anyhow!(
                "Second value (sample type) should be one of: uint8, int16, int32, float32."
            )),
        }?;

        let channels = match splits[2].strip_suffix("ch") {
            Some(channels) => match channels.parse::<u16>() {
                Ok(channels) => Ok(channels as u32),
                Err(_) => {
                    Err(anyhow::anyhow!("Third value (channels) should have form \"<uint>ch\"."))
                }
            },
            None => Err(anyhow::anyhow!("Channel argument should have form \"<uint>ch\".")),
        }?;

        Ok(Self {
            frames_per_second: frame_rate,
            sample_type: AudioSampleFormat::from(sample_type),
            channels,
        })
    }
}

#[derive(Debug, Eq, PartialEq, Clone)]
pub enum CommandSampleType {
    Uint8,
    Int16,
    Int32,
    Float32,
}

impl FromStr for CommandSampleType {
    type Err = anyhow::Error;
    fn from_str(s: &str) -> Result<Self, Error> {
        match s {
            "uint8" => Ok(CommandSampleType::Uint8),
            "int16" => Ok(CommandSampleType::Int16),
            "int32" => Ok(CommandSampleType::Int32),
            "float32" => Ok(CommandSampleType::Float32),
            _ => Err(anyhow::anyhow!("Invalid sampletype: {}.", s)),
        }
    }
}

impl From<CommandSampleType> for fidl_fuchsia_media::AudioSampleFormat {
    fn from(item: CommandSampleType) -> fidl_fuchsia_media::AudioSampleFormat {
        match item {
            CommandSampleType::Uint8 => fidl_fuchsia_media::AudioSampleFormat::Unsigned8,
            CommandSampleType::Int16 => fidl_fuchsia_media::AudioSampleFormat::Signed16,
            CommandSampleType::Int32 => fidl_fuchsia_media::AudioSampleFormat::Signed24In32,
            CommandSampleType::Float32 => fidl_fuchsia_media::AudioSampleFormat::Float,
        }
    }
}

/// Parses a Duration from string.
pub fn parse_duration(value: &str) -> Result<Duration, String> {
    let re = Regex::new(DURATION_REGEX).unwrap();
    let captures = re
        .captures(&value)
        .ok_or(format!("Durations must be specified in the form {}.", DURATION_REGEX))?;
    let number: u64 = captures[1].parse().unwrap();
    let unit = &captures[2];

    match unit {
        "ms" => Ok(Duration::from_millis(number)),
        "s" => Ok(Duration::from_secs(number)),
        "m" => Ok(Duration::from_secs(number * 60)),
        "h" => Ok(Duration::from_secs(number * 3600)),
        _ => Err(format!(
            "Invalid duration string \"{}\"; must be of the form {}.",
            value, DURATION_REGEX
        )),
    }
}

////////////////////////////////////////////////////////////////////////////////
// tests
#[cfg(test)]
pub mod test {
    use super::*;

    fn example_formats() -> Vec<Format> {
        vec![
            Format {
                frames_per_second: 48000,
                sample_type: AudioSampleFormat::Unsigned8,
                channels: 2,
            },
            Format { frames_per_second: 44100, sample_type: AudioSampleFormat::Float, channels: 1 },
        ]
    }

    #[test]
    fn test_format_parse() {
        let example_formats = example_formats();

        pretty_assertions::assert_eq!(
            example_formats[0],
            Format::from_str("48000,uint8,2ch").unwrap()
        );

        pretty_assertions::assert_eq!(
            example_formats[1],
            Format::from_str("44100,float32,1ch").unwrap()
        );

        // malformed inputs
        assert!(Format::from_str("44100,float,1ch").is_err());

        assert!(Format::from_str("44100").is_err());

        assert!(Format::from_str("44100,float32,1").is_err());

        assert!(Format::from_str("44100,float32").is_err());

        assert!(Format::from_str(",,").is_err());
    }
}