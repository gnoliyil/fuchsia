// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Representation of the virtual_device metadata.

mod manifest;
mod v1;

pub use manifest::*;
pub use v1::*;

use anyhow::{Context, Result};
use camino::Utf8Path;
use serde::{Deserialize, Serialize};
use std::fs::File;

const VIRTUAL_DEVICE_SCHEMA_V1: &str =
    "http://fuchsia.com/schemas/sdk/virtual_device-93A41932.json";

/// Private helper for deserializing the virtual device.
#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
#[serde(untagged)]
enum SerializationHelper {
    V1 { schema_id: String, data: VirtualDeviceV1 },
}

/// Versioned virtual device specification.
#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
#[serde(untagged)]
pub enum VirtualDevice {
    V1(VirtualDeviceV1),
}

impl VirtualDevice {
    /// Load a VirtualDevice from a path on disk.
    pub fn try_load_from(path: impl AsRef<Utf8Path>) -> Result<Self> {
        let name = path.as_ref().file_stem().with_context(|| {
            format!("Can't determine device name based on provided path: '{}'", path.as_ref())
        })?;
        let file = File::open(path.as_ref())
            .with_context(|| format!("opening virtual device: {:?}", path.as_ref()))?;
        let helper: SerializationHelper =
            serde_json::from_reader(file).context("parsing virtual device")?;
        let device = match helper {
            SerializationHelper::V1 { schema_id: _, mut data } => {
                // Use the filename as the device name instead of the name
                // originally in the file.
                // TODO: Why are we doing this?
                data.name = name.to_string();
                VirtualDevice::V1(data)
            }
        };
        Ok(device)
    }

    /// Write a virtual device to disk at `path`.
    pub fn write(&self, path: impl AsRef<Utf8Path>) -> Result<()> {
        let helper = match self {
            Self::V1(data) => SerializationHelper::V1 {
                schema_id: VIRTUAL_DEVICE_SCHEMA_V1.into(),
                data: data.clone(),
            },
        };
        let file = File::create(path.as_ref()).context("creating virtual device file")?;
        serde_json::to_writer(file, &helper).context("writing virtual device file")?;
        Ok(())
    }

    /// Returns VirtualDevice entry name.
    pub fn name(&self) -> &str {
        match self {
            Self::V1(device) => &device.name.as_str(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    use std::fs::File;
    use tempfile::TempDir;

    #[test]
    fn test_parse_v1() {
        let tmp = TempDir::new().unwrap();
        let vd_path = Utf8Path::from_path(tmp.path()).unwrap().join("virtual_device.json");
        let vd_file = File::create(&vd_path).unwrap();
        serde_json::to_writer(
            &vd_file,
            &json!({
                "schema_id": "http://fuchsia.com/schemas/sdk/virtual_device-93A41932.json",
                "data": {
                    "name": "generic-x64",
                    "type": "virtual_device",
                    "hardware": {
                        "audio": {
                            "model": "hda"
                        },
                        "cpu": {
                            "arch": "x64"
                        },
                        "inputs": {
                            "pointing_device": "touch"
                        },
                        "window_size": {
                            "width": 640,
                            "height": 480,
                            "units": "pixels"
                        },
                        "memory": {
                            "quantity": 1,
                            "units": "gigabytes"
                        },
                        "storage": {
                            "quantity": 1,
                            "units": "gigabytes"
                        }
                    },
                    "start_up_args_template": "/path/to/args"
                }
            }),
        )
        .unwrap();
        let vd = VirtualDevice::try_load_from(vd_path).unwrap();
        assert!(matches!(vd, VirtualDevice::V1 { .. }));
        assert_eq!(vd.name(), "virtual_device");
    }
}
