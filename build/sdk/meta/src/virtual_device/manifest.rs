// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Representation of the virtual device manifest.

use crate::VirtualDevice;
use anyhow::{anyhow, bail, Context, Result};
use camino::Utf8PathBuf;
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::fs::File;

/// A Virtual Device Manifest contains metadata about the virtual
/// devices contained in the current product bundle.
#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct VirtualDeviceManifest {
    /// The virtual device in this bundle which is the recommended default for
    /// emulation if the user doesn't specify one explicitly. This value is
    /// meant to be used as a key into the device_paths map.
    #[serde(default)]
    pub recommended: Option<String>,

    /// The list of devices included in this bundle. Each entry is the path to
    /// the virtual device specification file for that device, relative to the
    /// base of the product bundle.
    #[serde(default)]
    pub device_paths: HashMap<String, Utf8PathBuf>,

    /// Path to the parent of the manifest file, since all virtual device file
    /// paths are relative to that file.
    #[serde(skip)]
    parent_dir_path: Utf8PathBuf,
}

impl Default for VirtualDeviceManifest {
    fn default() -> Self {
        Self {
            recommended: None,
            device_paths: HashMap::new(),
            parent_dir_path: Utf8PathBuf::new(),
        }
    }
}

impl VirtualDeviceManifest {
    /// Given a path to a file, attempt to deserialize its contents as a VirtualDeviceManifest.
    pub fn from_path(path: &Option<Utf8PathBuf>) -> Result<VirtualDeviceManifest> {
        let path = match path {
            None => return Ok(VirtualDeviceManifest::default()),
            Some(path) => path,
        };
        if !path.is_file() {
            bail!("Path '{}' does not refer to a Virtual Device Manifest file.", path);
        }
        let parent = path.parent().with_context(|| format!("path '{}' has no parent", path))?;
        let file = File::open(&path).with_context(|| format!("open '{}'", path))?;
        match serde_json::from_reader::<File, VirtualDeviceManifest>(file).map_err(|e| e.into()) {
            Ok(mut manifest) => {
                manifest.parent_dir_path = parent.into();
                Ok(manifest)
            }
            Err(e) => Err(e),
        }
    }

    /// Given a path to a file, attempt to deserialize its contents as a VirtualDevice.
    fn parse_virtual_device_file(path: &Utf8PathBuf) -> Result<VirtualDevice> {
        let name = match path.file_stem() {
            None => bail!("Can't determine device name based on provided path: '{}'", path),
            Some(n) => n,
        };
        let file = File::open(&path).with_context(|| format!("open '{}'", path))?;
        let mut virtual_device: VirtualDevice =
            serde_json::from_reader(&file).context("parsing virtual device from_reader")?;
        match virtual_device {
            VirtualDevice::V1(ref mut device) => {
                device.name = name.to_string();
            }
        }
        Ok(virtual_device)
    }

    /// Given a VirtualDevice, ensure the template path is specified relative to this manifest's
    /// local filesystem path.
    fn adjust_template_path(&self, mut device: VirtualDevice) -> VirtualDevice {
        match device {
            VirtualDevice::V1(ref mut v) => {
                let template = &v.start_up_args_template;
                v.start_up_args_template = self.parent_dir_path.join(template);
            }
        }
        device
    }

    /// Return a vector of the Virtual Device names listed in this manifest.
    pub fn device_names(&self) -> Vec<String> {
        self.device_paths.keys().cloned().collect()
    }

    /// Given the name of a Virtual Device, look up the path to that device in the manifest and
    /// attempt to deserialize its details and return a VirtualDevice object. Returns an error if
    /// the requested device is not included in this manifest, if the path associated with that
    /// device is invalid, or if it cannor be deserialized correctly.
    pub fn device(&self, name: &str) -> Result<VirtualDevice> {
        let path = self
            .device_paths
            .get(name)
            .ok_or_else(|| anyhow!("Device {} is not listed in this Product Bundle.", name))?;
        let device = Self::parse_virtual_device_file(&self.parent_dir_path.join(path))
            .with_context(|| {
                format!("parse virtual device file '{}'", self.parent_dir_path.join(path))
            })?;
        Ok(self.adjust_template_path(device))
    }

    /// If this manifest has a "recommended" virtual device specified, deserialize that file and
    /// return the resulting VirtualDevice. Returns:
    ///  - Err(): if deserialization fails,
    ///           or the name doesn't match a device in the manifest,
    ///           or the path to that device is invalid.
    ///  - Ok(None): if no "recommended" value is defined.
    ///  - Ok(Some(device)): on success.
    pub fn default_device(&self) -> Result<Option<VirtualDevice>> {
        let rec = match &self.recommended {
            None => return Ok(None),
            Some(rec) => rec,
        };
        let path = self
            .device_paths
            .get(rec)
            .ok_or_else(|| anyhow!("Default of '{}' was not found in the device manifest.", rec))?;
        Self::parse_virtual_device_file(&self.parent_dir_path.join(path))
            .map(|d| self.adjust_template_path(d))
            .map(|d| Some(d))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::virtual_device::VirtualDeviceV1;
    use std::fs::{create_dir_all, File};
    use std::io::Write;
    use tempfile::TempDir;

    const VIRTUAL_DEVICE_VALID: &str = include_str!("../../test_data/virtual_device.json");
    const VIRTUAL_DEVICE_INVALID: &str = r#"{ "something": "bad" }"#;
    const VIRTUAL_DEVICE_MANIFEST_SINGLE: &str =
        include_str!("../../test_data/single_vd_manifest.json");

    #[test]
    fn test_get_device_names_empty() -> Result<()> {
        let manifest = VirtualDeviceManifest {
            recommended: None,
            device_paths: HashMap::new(),
            parent_dir_path: Utf8PathBuf::from(""),
        };
        let result = manifest.device_names();
        assert!(result.is_empty());
        Ok(())
    }

    #[test]
    fn test_get_device_names_some() -> Result<()> {
        let manifest = VirtualDeviceManifest {
            recommended: None,
            device_paths: [
                ("device1".into(), Utf8PathBuf::new()),
                ("device2".into(), Utf8PathBuf::new()),
                ("device3".into(), Utf8PathBuf::new()),
            ]
            .into(),
            parent_dir_path: Utf8PathBuf::new(),
        };

        let result = manifest.device_names();
        assert!(!result.is_empty());
        assert_eq!(result.len(), 3);
        assert!(result.contains(&"device1".into()));
        assert!(result.contains(&"device2".into()));
        assert!(result.contains(&"device3".into()));

        Ok(())
    }

    #[test]
    fn test_get_device_none() -> Result<()> {
        let manifest = VirtualDeviceManifest {
            recommended: None,
            device_paths: HashMap::new(),
            parent_dir_path: Utf8PathBuf::from(""),
        };
        let result = manifest.device("device");
        assert!(result.is_err());
        Ok(())
    }

    #[test]
    fn test_get_device_not_in_list() -> Result<()> {
        let manifest = VirtualDeviceManifest {
            recommended: None,
            device_paths: [
                ("device1".into(), Utf8PathBuf::new()),
                ("device2".into(), Utf8PathBuf::new()),
                ("device3".into(), Utf8PathBuf::new()),
            ]
            .into(),
            parent_dir_path: Utf8PathBuf::new(),
        };

        let result = manifest.device("something_else");
        assert!(result.is_err());
        Ok(())
    }

    #[test]
    fn test_get_device_bad_path() -> Result<()> {
        let tempdir = TempDir::new().expect("temp dir");
        let vd_dir = tempdir.path().join("virtual_devices");
        create_dir_all(&vd_dir).expect("make_dir virtual_devices");

        let vd_file_path1 = vd_dir.join("device1.json");
        let vd_file_path2 = vd_dir.join("device2.json");
        let vd_file_path3 = vd_dir.join("extra/path/device3.json");

        let mut file1 = File::create(&vd_file_path1).expect("create device1.json");
        // Don't create the file for device2
        create_dir_all(&vd_dir.join("extra/path")).expect("make_dir extra/path/");
        let mut file3 = File::create(&vd_file_path3).expect("create device3.json");

        file1.write_all(VIRTUAL_DEVICE_VALID.as_bytes())?;
        file3.write_all(VIRTUAL_DEVICE_VALID.as_bytes())?;

        let manifest = VirtualDeviceManifest {
            recommended: None,
            device_paths: [
                (
                    "device1".into(),
                    Utf8PathBuf::from_path_buf(vd_file_path1)
                        .expect("Couldn't get utf8 path buf, device1"),
                ),
                (
                    "device2".into(),
                    Utf8PathBuf::from_path_buf(vd_file_path2)
                        .expect("Couldn't get utf8 path buf, device2"),
                ),
                (
                    "device3".into(),
                    Utf8PathBuf::from_path_buf(vd_file_path3)
                        .expect("Couldn't get utf8 path buf, device3"),
                ),
            ]
            .into(),
            parent_dir_path: Utf8PathBuf::from_path_buf(vd_dir)
                .expect("Couldn't get utf8 path buf, local"),
        };

        let result = manifest.device("device2");
        assert!(result.is_err());
        Ok(())
    }

    #[test]
    fn test_get_device_bad_file() -> Result<()> {
        let tempdir = TempDir::new().expect("temp dir");
        let vd_dir = tempdir.path().join("virtual_devices");
        create_dir_all(&vd_dir).expect("make_dir virtual_devices");

        let vd_file_path1 = vd_dir.join("device1.json");
        let vd_file_path2 = vd_dir.join("device2.json");
        let vd_file_path3 = vd_dir.join("extra/path/device3.json");

        let mut file1 = File::create(&vd_file_path1).expect("create device1.json");
        File::create(&vd_file_path2).expect("create device2.json");
        create_dir_all(&vd_dir.join("extra/path")).expect("make_dir extra/path/");
        let mut file3 = File::create(&vd_file_path3).expect("create device3.json");

        file1.write_all(VIRTUAL_DEVICE_VALID.as_bytes())?;
        // Don't write the device contents to file2
        file3.write_all(VIRTUAL_DEVICE_VALID.as_bytes())?;

        let manifest = VirtualDeviceManifest {
            recommended: None,
            device_paths: [
                (
                    "device1".into(),
                    Utf8PathBuf::from_path_buf(vd_file_path1)
                        .expect("Couldn't get utf8 path buf, device1"),
                ),
                (
                    "device2".into(),
                    Utf8PathBuf::from_path_buf(vd_file_path2)
                        .expect("Couldn't get utf8 path buf, device2"),
                ),
                (
                    "device3".into(),
                    Utf8PathBuf::from_path_buf(vd_file_path3)
                        .expect("Couldn't get utf8 path buf, device3"),
                ),
            ]
            .into(),
            parent_dir_path: Utf8PathBuf::from_path_buf(vd_dir)
                .expect("Couldn't get utf8 path buf, local"),
        };

        let result = manifest.device("device2");
        assert!(result.is_err());
        Ok(())
    }

    #[test]
    fn test_get_device_ok() -> Result<()> {
        let tempdir = TempDir::new().expect("temp dir");
        let vd_dir = tempdir.path().join("virtual_devices");
        create_dir_all(&vd_dir).expect("make_dir virtual_devices");

        let vd_file_path1 = vd_dir.join("device1.json");
        let vd_file_path2 = vd_dir.join("device2.json");
        let vd_file_path3 = vd_dir.join("extra/path/device3.json");

        let mut file1 = File::create(&vd_file_path1).expect("create device1.json");
        let mut file2 = File::create(&vd_file_path2).expect("create device2.json");
        create_dir_all(&vd_dir.join("extra/path")).expect("make_dir extra/path/");
        let mut file3 = File::create(&vd_file_path3).expect("create device3.json");

        file1.write_all(VIRTUAL_DEVICE_VALID.as_bytes())?;
        file2.write_all(VIRTUAL_DEVICE_VALID.as_bytes())?;
        file3.write_all(VIRTUAL_DEVICE_VALID.as_bytes())?;

        let manifest = VirtualDeviceManifest {
            recommended: None,
            device_paths: [
                (
                    "device1".into(),
                    Utf8PathBuf::from_path_buf(vd_file_path1)
                        .expect("Couldn't get utf8 path buf, device1"),
                ),
                (
                    "device2".into(),
                    Utf8PathBuf::from_path_buf(vd_file_path2)
                        .expect("Couldn't get utf8 path buf, device2"),
                ),
                (
                    "device3".into(),
                    Utf8PathBuf::from_path_buf(vd_file_path3)
                        .expect("Couldn't get utf8 path buf, device3"),
                ),
            ]
            .into(),
            parent_dir_path: Utf8PathBuf::from_path_buf(vd_dir)
                .expect("Couldn't get utf8 path buf, local"),
        };

        let result = manifest.device("device1");
        assert!(result.is_ok(), "{:?}", result.unwrap_err());
        let result = result.unwrap();
        assert!(matches!(result, VirtualDevice::V1(VirtualDeviceV1 { .. })));

        let result = manifest.device("device2");
        assert!(result.is_ok(), "{:?}", result.unwrap_err());
        let result = result.unwrap();
        assert!(matches!(result, VirtualDevice::V1(VirtualDeviceV1 { .. })));

        let result = manifest.device("device3");
        assert!(result.is_ok(), "{:?}", result.unwrap_err());
        let result = result.unwrap();
        assert!(matches!(result, VirtualDevice::V1(VirtualDeviceV1 { .. })));

        Ok(())
    }

    #[test]
    fn test_get_default_none() -> Result<()> {
        let manifest = VirtualDeviceManifest {
            recommended: None,
            device_paths: HashMap::new(),
            parent_dir_path: Utf8PathBuf::new(),
        };
        let result = manifest.default_device();
        assert!(result.is_ok());
        assert!(result.unwrap().is_none());

        Ok(())
    }

    #[test]
    fn test_get_default_some() -> Result<()> {
        let tempdir = TempDir::new().expect("temp dir");

        let vd_dir = tempdir.path().join("virtual_devices");
        create_dir_all(&vd_dir).expect("make_dir virtual_devices");

        let vd_file_path1 = vd_dir.join("device1.json");
        let vd_file_path2 = vd_dir.join("device2.json");
        let vd_file_path3 = vd_dir.join("extra/path/device3.json");

        let mut file1 = File::create(&vd_file_path1).expect("create device1.json");
        let mut file2 = File::create(&vd_file_path2).expect("create device2.json");
        create_dir_all(&vd_dir.join("extra/path")).expect("make_dir extra/path/");
        let mut file3 = File::create(&vd_file_path3).expect("create device3.json");

        file1.write_all(VIRTUAL_DEVICE_VALID.as_bytes())?;
        file2.write_all(VIRTUAL_DEVICE_VALID.as_bytes())?;
        file3.write_all(VIRTUAL_DEVICE_VALID.as_bytes())?;

        let manifest = VirtualDeviceManifest {
            recommended: Some("device2".to_string()),
            device_paths: [
                (
                    "device1".into(),
                    Utf8PathBuf::from_path_buf(vd_file_path1)
                        .expect("Couldn't get utf8 path buf, device1"),
                ),
                (
                    "device2".into(),
                    Utf8PathBuf::from_path_buf(vd_file_path2)
                        .expect("Couldn't get utf8 path buf, device2"),
                ),
                (
                    "device3".into(),
                    Utf8PathBuf::from_path_buf(vd_file_path3)
                        .expect("Couldn't get utf8 path buf, device3"),
                ),
            ]
            .into(),
            parent_dir_path: Utf8PathBuf::from_path_buf(vd_dir)
                .expect("Couldn't get utf8 path buf, parent_dir_path"),
        };

        let result = manifest.default_device();

        assert!(result.is_ok());
        let result = result.unwrap();
        assert!(result.is_some());
        let result = result.unwrap();
        assert_eq!(result.name(), "device2");

        Ok(())
    }

    #[test]
    fn test_from_path_none() -> Result<()> {
        let result = VirtualDeviceManifest::from_path(&None);
        assert!(result.is_ok());
        assert_eq!(result.unwrap(), VirtualDeviceManifest::default());
        Ok(())
    }

    #[test]
    fn test_from_path_ok() -> Result<()> {
        let tempdir = TempDir::new().expect("temp dir");
        let vd_dir = tempdir.path().join("virtual_devices");
        create_dir_all(&vd_dir).expect("make_dir virtual_devices");

        let manifest_path = vd_dir.join("device.json");
        let mut file = File::create(&manifest_path).expect("create device.json");
        file.write_all(VIRTUAL_DEVICE_MANIFEST_SINGLE.as_bytes())?;

        let result = VirtualDeviceManifest::from_path(&Some(
            Utf8PathBuf::from_path_buf(manifest_path)
                .expect("Couldn't get utf8 path buf, manifest"),
        ));
        assert!(result.is_ok());
        let result = result.unwrap();
        assert_eq!(
            result,
            VirtualDeviceManifest {
                device_paths: [("device".to_string(), "device.json".into())].into(),
                recommended: Some("device".to_string()),
                parent_dir_path: Utf8PathBuf::from_path_buf(vd_dir)
                    .expect("Couldn't get utf8 path buf, local path"),
            }
        );
        Ok(())
    }

    #[test]
    fn test_parse_virtual_device_file_invalid() -> Result<()> {
        let tempdir = TempDir::new().expect("temp dir");
        let vd_dir = tempdir.path().join("virtual_devices");
        create_dir_all(&vd_dir).expect("make_dir virtual_devices");

        let vd_path = vd_dir.join("device.json");
        let mut file = File::create(&vd_path).expect("create device.json");
        file.write_all(VIRTUAL_DEVICE_INVALID.as_bytes())?;

        let result = VirtualDeviceManifest::parse_virtual_device_file(
            &Utf8PathBuf::from_path_buf(vd_path).expect("Couldn't get utf8 path buf, vd path"),
        );
        assert!(result.is_err());
        Ok(())
    }

    #[test]
    fn test_parse_virtual_device_file_valid() -> Result<()> {
        let tempdir = TempDir::new().expect("temp dir");
        let vd_dir = tempdir.path().join("virtual_devices");
        create_dir_all(&vd_dir).expect("make_dir virtual_devices");

        let vd_path = vd_dir.join("device.json");
        let mut file = File::create(&vd_path).expect("create device.json");
        file.write_all(VIRTUAL_DEVICE_VALID.as_bytes())?;

        let result = VirtualDeviceManifest::parse_virtual_device_file(
            &Utf8PathBuf::from_path_buf(vd_path).expect("Couldn't get utf8 path buf, vd path"),
        );
        assert!(result.is_ok(), "{:?}", result.unwrap_err());
        assert_eq!(result.unwrap().name(), "device");
        Ok(())
    }
}
