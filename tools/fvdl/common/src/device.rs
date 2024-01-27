// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::args::StartCommand;
use serde::Deserialize;
use std::fs::File;
use std::io::BufReader;

// TODO(fxbug.dev/84803): This will need to move to the parsing library once we have a schema.
// Note: this struct is a holding place for inputs from device manifest files, which are defined
// by a fixed schema in //build/sdk/meta. Any changes to one must be reflected in the other.
#[derive(Debug, Deserialize)]
pub struct DeviceSpec {
    pub audio: bool,
    pub image_size: String,
    pub pointing_device: String,
    pub ram_mb: usize,
    pub window_height: usize,
    pub window_width: usize,
}

impl DeviceSpec {
    /// Returns None if the image size is invalid or unspecified.
    pub fn image_size_bytes(&self) -> Option<u64> {
        if self.image_size.is_empty() {
            return None;
        }
        let (multiplier, consumed) = match self.image_size.chars().last().unwrap() {
            'G' => (1024 * 1024 * 1024, 1),
            'M' => (1024 * 1024, 1),
            'K' => (1024, 1),
            _ => (1, 0),
        };
        let value: u64 = self.image_size[..self.image_size.len() - consumed].parse().ok()?;
        value.checked_mul(multiplier)
    }
}

fn default_audio() -> bool {
    true
}

fn default_window_height() -> usize {
    800
}

fn default_window_width() -> usize {
    1280
}

// Note the value for ram should match the defaults used in `fx emu` (//tools/devshell/emu)
// and in `fx qemu` (//zircon/scripts/run-zircon).
fn default_ram_mb() -> usize {
    8192
}

fn default_image_size() -> String {
    "2G".to_string()
}

fn default_pointing_device() -> String {
    "touch".to_string()
}

impl std::default::Default for DeviceSpec {
    fn default() -> Self {
        DeviceSpec {
            audio: default_audio(),
            image_size: default_image_size(),
            pointing_device: default_pointing_device(),
            ram_mb: default_ram_mb(),
            window_height: default_window_height(),
            window_width: default_window_width(),
        }
    }
}

impl DeviceSpec {
    fn get_values_from_flags(&mut self, cmd: &StartCommand) {
        self.audio = cmd.audio.or(Some(self.audio)).unwrap();
        self.image_size = cmd.image_size.as_ref().unwrap_or(&self.image_size).to_string();
        self.pointing_device =
            cmd.pointing_device.as_ref().unwrap_or(&self.pointing_device).to_string();
        self.ram_mb = cmd.ram_mb.or(Some(self.ram_mb)).unwrap();
        self.window_height = cmd.window_height.or(Some(self.window_height)).unwrap();
        self.window_width = cmd.window_width.or(Some(self.window_width)).unwrap();
    }

    pub fn from_manifest(cmd: &StartCommand) -> Result<DeviceSpec, anyhow::Error> {
        let mut spec = match &cmd.device_spec {
            None => DeviceSpec::default(),
            Some(path) => {
                // Open the file in read-only mode with buffer.
                let file = File::open(path)?;
                let reader = BufReader::new(file);

                // Read the JSON contents of the file as an instance of `DeviceSpec`.
                serde_json::from_reader(reader)?
            }
        };
        spec.get_values_from_flags(cmd);
        return Ok(spec);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_convert_start_cmd_to_device_spec() {
        let start_command = &StartCommand {
            audio: Some(false),
            image_size: Some("512M".to_string()),
            pointing_device: Some("mouse".to_string()),
            ram_mb: Some(16392),
            window_height: Some(480),
            window_width: Some(640),
            ..Default::default()
        };
        let mut device_spec: DeviceSpec = DeviceSpec::default();
        assert_eq!(device_spec.audio, default_audio());
        assert_eq!(device_spec.image_size, default_image_size());
        assert_eq!(device_spec.pointing_device, default_pointing_device());
        assert_eq!(device_spec.ram_mb, default_ram_mb());
        assert_eq!(device_spec.window_height, default_window_height());
        assert_eq!(device_spec.window_width, default_window_width());

        device_spec.get_values_from_flags(start_command);
        assert_eq!(device_spec.audio, false);
        assert_eq!(device_spec.image_size, "512M");
        assert_eq!(device_spec.pointing_device, "mouse");
        assert_eq!(device_spec.ram_mb, 16392);
        assert_eq!(device_spec.window_height, 480);
        assert_eq!(device_spec.window_width, 640);
    }

    #[test]
    fn test_image_size_parsing() {
        let create_spec =
            |img_size: &str| DeviceSpec { image_size: img_size.to_string(), ..Default::default() };

        assert_eq!(create_spec("2G").image_size_bytes(), Some(2 * 1024 * 1024 * 1024));
        assert_eq!(create_spec("3M").image_size_bytes(), Some(3 * 1024 * 1024));
        assert_eq!(create_spec("44K").image_size_bytes(), Some(44 * 1024));
        assert_eq!(create_spec("1023").image_size_bytes(), Some(1023));
        assert_eq!(create_spec("").image_size_bytes(), None);
        assert_eq!(create_spec("nan").image_size_bytes(), None);
        assert_eq!(create_spec("xG").image_size_bytes(), None);
        assert_eq!(create_spec("2nan000G").image_size_bytes(), None);
    }
}
