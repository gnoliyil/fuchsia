// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        device::{
            constants::{
                BLOBFS_PARTITION_LABEL, BLOBFS_TYPE_GUID, BOOTPART_DRIVER_PATH,
                DATA_PARTITION_LABEL, DATA_TYPE_GUID, FVM_DRIVER_PATH, GPT_DRIVER_PATH,
                LEGACY_DATA_PARTITION_LABEL, MBR_DRIVER_PATH, NAND_BROKER_DRIVER_PATH,
            },
            Device,
        },
        environment::{Environment, ServeFilesystemStatus},
    },
    anyhow::Error,
    async_trait::async_trait,
    fs_management::format::DiskFormat,
    std::{collections::BTreeMap, ops::Bound},
};

#[async_trait]
pub trait Matcher: Send {
    /// Tries to match this device against this matcher. Matching should be infallible.
    async fn match_device(&self, device: &mut dyn Device) -> bool;

    /// Process this device as the format this matcher is for. This is called when this matcher
    /// returns true during matching. This step is fallible - if a device matched a matcher, but
    /// then this step fails, we stop matching and bubble up the error.
    async fn process_device(
        &mut self,
        device: &mut dyn Device,
        env: &mut dyn Environment,
    ) -> Result<(), Error>;

    /// This is called when a device appears that is a child of an already matched device.  It can
    /// be anywhere within the hierarchy, so not necessarily an immediate child.  Devices will be
    /// matched as children before being matched against global matches.
    ///
    /// Matchers are responsible for calling both match_device and process_device on their
    /// children.
    async fn match_and_process_child(
        &mut self,
        _device: &mut dyn Device,
        _env: &mut dyn Environment,
        _parent_path: &str,
    ) -> Result<bool, Error> {
        // By default, matchers don't match children.
        Ok(false)
    }
}

pub struct Matchers {
    matchers: Vec<Box<dyn Matcher>>,

    matched: BTreeMap<String, usize>,
}

impl Matchers {
    /// Create a new set of matchers. This essentially describes the expected partition layout for
    /// a device.
    pub fn new(config: &fshost_config::Config, ramdisk_path: Option<String>) -> Self {
        let mut matchers = Vec::<Box<dyn Matcher>>::new();

        if config.bootpart {
            matchers.push(Box::new(BootpartMatcher::new()));
        }
        if config.nand {
            matchers.push(Box::new(NandMatcher::new()));
        }
        if config.fxfs_blob {
            // TODO(https://fxbug.dev/124455): Figure out netboot semantics for fxblob.
            if !config.netboot {
                matchers.push(Box::new(FxblobMatcher::new(
                    DiskFormat::Fxfs,
                    if config.ramdisk_image { ramdisk_path } else { None },
                )));
            }
        } else {
            // If ramdisk_image is true but we don't actually have a ramdisk launching, we skip
            // making the fvm+blobfs+data matcher entirely.
            if !config.ramdisk_image || ramdisk_path.is_some() {
                let mut fvm_matcher = Box::new(PartitionMapMatcher::new(
                    DiskFormat::Fvm,
                    false,
                    FVM_DRIVER_PATH,
                    "/fvm",
                    if config.ramdisk_image { ramdisk_path } else { None },
                ));
                if !config.netboot {
                    if config.blobfs {
                        fvm_matcher.child_matchers.push(Box::new(BlobfsMatcher::new()));
                    }
                    if config.data {
                        fvm_matcher.child_matchers.push(Box::new(DataMatcher::new()));
                    }
                }
                if config.fvm || !fvm_matcher.child_matchers.is_empty() {
                    matchers.push(fvm_matcher);
                }
            }

            if config.fvm && config.ramdisk_image {
                // Add another matcher for the non-ramdisk version of fvm.
                let non_ramdisk_fvm_matcher = Box::new(PartitionMapMatcher::new(
                    DiskFormat::Fvm,
                    false,
                    FVM_DRIVER_PATH,
                    "/fvm",
                    None,
                ));
                matchers.push(non_ramdisk_fvm_matcher);
            }
        }

        let gpt_matcher =
            Box::new(PartitionMapMatcher::new(DiskFormat::Gpt, false, GPT_DRIVER_PATH, "", None));
        if config.gpt || !gpt_matcher.child_matchers.is_empty() {
            matchers.push(gpt_matcher);
        }

        if config.gpt_all {
            matchers.push(Box::new(PartitionMapMatcher::new(
                DiskFormat::Gpt,
                true,
                GPT_DRIVER_PATH,
                "",
                None,
            )));
        }

        if config.mbr {
            matchers.push(Box::new(PartitionMapMatcher::new(
                DiskFormat::Mbr,
                true,
                MBR_DRIVER_PATH,
                "",
                None,
            )))
        }

        Matchers { matchers, matched: BTreeMap::new() }
    }

    /// Using the set of matchers we created, figure out if this block device matches any of our
    /// expected partitions. If it does, return the information needed to launch the filesystem,
    /// such as the component url or the shared library to pass to the driver binding.
    pub async fn match_device(
        &mut self,
        device: &mut dyn Device,
        env: &mut dyn Environment,
    ) -> Result<bool, Error> {
        for (path, &index) in self
            .matched
            .range::<str, _>((Bound::Unbounded, Bound::Excluded(device.topological_path())))
            .rev()
        {
            if device.topological_path().starts_with(path) {
                if self.matchers[index].match_and_process_child(device, env, path).await? {
                    self.matched.insert(device.topological_path().to_string(), index);
                    return Ok(true);
                }
            }
        }
        for (index, m) in self.matchers.iter_mut().enumerate() {
            if m.match_device(device).await {
                m.process_device(device, env).await?;
                self.matched.insert(device.topological_path().to_string(), index);
                return Ok(true);
            }
        }
        Ok(false)
    }
}

// Matches Bootpart devices.
struct BootpartMatcher();

impl BootpartMatcher {
    fn new() -> Self {
        BootpartMatcher()
    }
}

#[async_trait]
impl Matcher for BootpartMatcher {
    async fn match_device(&self, device: &mut dyn Device) -> bool {
        device
            .get_block_info()
            .await
            .map_or(false, |info| info.flags.contains(fidl_fuchsia_hardware_block::Flag::BOOTPART))
    }

    async fn process_device(
        &mut self,
        device: &mut dyn Device,
        env: &mut dyn Environment,
    ) -> Result<(), Error> {
        env.attach_driver(device, BOOTPART_DRIVER_PATH).await
    }
}

// Matches Nand devices.
struct NandMatcher();

impl NandMatcher {
    fn new() -> Self {
        NandMatcher()
    }
}

#[async_trait]
impl Matcher for NandMatcher {
    async fn match_device(&self, device: &mut dyn Device) -> bool {
        device.is_nand()
    }

    async fn process_device(
        &mut self,
        device: &mut dyn Device,
        env: &mut dyn Environment,
    ) -> Result<(), Error> {
        env.attach_driver(device, NAND_BROKER_DRIVER_PATH).await
    }
}

// Matches against a data partition that exists independent of FVM using the DiskFormat.
struct FxblobMatcher {
    // The content format expected.
    content_format: DiskFormat,
    // If this partition is required to exist on a ramdisk, then this contains the prefix it should
    // have.
    ramdisk_required: Option<String>,
}

impl FxblobMatcher {
    fn new(content_format: DiskFormat, ramdisk_required: Option<String>) -> Self {
        Self { content_format, ramdisk_required }
    }
}

#[async_trait]
impl Matcher for FxblobMatcher {
    async fn match_device(&self, device: &mut dyn Device) -> bool {
        if let Some(ramdisk_prefix) = &self.ramdisk_required {
            if !device.topological_path().starts_with(ramdisk_prefix) {
                return false;
            }
        }
        device.content_format().await.ok() == Some(self.content_format)
    }

    async fn process_device(
        &mut self,
        device: &mut dyn Device,
        env: &mut dyn Environment,
    ) -> Result<(), Error> {
        env.mount_fxblob(device).await?;
        env.mount_blob_volume().await?;
        env.mount_data_volume().await?;
        Ok(())
    }
}

// Matches partition maps. Matching is done using content sniffing. `child_matchers` contain
// matchers that will match against partitions of partition maps.
struct PartitionMapMatcher {
    // The content format expected.
    content_format: DiskFormat,

    // If true, match against multiple devices. Otherwise, only the first is matched.
    allow_multiple: bool,

    // When matched, this driver is attached to the device.
    driver_path: &'static str,

    // The expected path suffix used in the topological path. For example, FVM uses an "fvm/"
    // suffix.
    path_suffix: &'static str,

    ramdisk_path: Option<String>,

    // The topological paths of all devices matched so far.
    device_paths: Vec<String>,

    // A list of matchers to use against partitions of matched devices.
    child_matchers: Vec<Box<dyn Matcher + Sync>>,
}

impl PartitionMapMatcher {
    fn new(
        content_format: DiskFormat,
        allow_multiple: bool,
        driver_path: &'static str,
        path_suffix: &'static str,
        ramdisk_path: Option<String>,
    ) -> Self {
        Self {
            content_format,
            allow_multiple,
            driver_path,
            path_suffix,
            ramdisk_path,
            device_paths: Vec::new(),
            child_matchers: Vec::new(),
        }
    }
}

#[async_trait]
impl Matcher for PartitionMapMatcher {
    async fn match_device(&self, device: &mut dyn Device) -> bool {
        if !self.allow_multiple && !self.device_paths.is_empty() {
            return false;
        }
        if let Some(ramdisk_prefix) = &self.ramdisk_path {
            if device.topological_path().starts_with(ramdisk_prefix) {
                tracing::info!(path = %device.path(), %ramdisk_prefix, "Found the fvm ramdisk");
            } else {
                return false;
            }
        }
        device.content_format().await.ok() == Some(self.content_format)
    }

    async fn process_device(
        &mut self,
        device: &mut dyn Device,
        env: &mut dyn Environment,
    ) -> Result<(), Error> {
        env.attach_driver(device, self.driver_path).await?;
        self.device_paths.push(device.topological_path().to_string());
        Ok(())
    }

    async fn match_and_process_child(
        &mut self,
        device: &mut dyn Device,
        env: &mut dyn Environment,
        parent_path: &str,
    ) -> Result<bool, Error> {
        let topological_path = device.topological_path();
        // If allow_multiple is not true, don't match children multiple times either.
        if !self.allow_multiple && self.device_paths.iter().any(|x| x == &topological_path) {
            return Ok(false);
        }
        // Only match against children that are immediate children.
        // Child partitions should have topological paths of the form:
        //   ...<suffix>/<partition-name>/block
        if let Some((head, file_name)) = topological_path.rsplit_once('/') {
            if file_name == "block" {
                if let Some(head) =
                    head.rsplit_once('/').and_then(|(head, _)| head.strip_suffix(self.path_suffix))
                {
                    if head == parent_path {
                        for m in self.child_matchers.iter_mut() {
                            if m.match_device(device).await {
                                m.process_device(device, env).await?;
                                self.device_paths.push(device.topological_path().to_string());
                                return Ok(true);
                            }
                        }
                    }
                }
            }
        }
        Ok(false)
    }
}

/// Matches a device with a given label and type guid. This is the common matching behavior between
/// all matchers that match against devices in partition tables. It itself isn't a fully functional
/// matcher - it doesn't know what to do with the device it matched - so it doesn't implement the
/// Matcher trait.
struct PartitionMatcher {
    label: &'static str,
    type_guid: &'static [u8; 16],
}

impl PartitionMatcher {
    fn new(label: &'static str, type_guid: &'static [u8; 16]) -> Self {
        Self { label, type_guid }
    }

    async fn match_device(&self, device: &mut dyn Device) -> bool {
        device.partition_label().await.ok() == Some(self.label)
            && device.partition_type().await.ok() == Some(self.type_guid)
    }
}

// Matches against a Blobfs partition (by checking for partition label and type GUID).
struct BlobfsMatcher(PartitionMatcher);

impl BlobfsMatcher {
    fn new() -> Self {
        Self(PartitionMatcher::new(BLOBFS_PARTITION_LABEL, &BLOBFS_TYPE_GUID))
    }
}

#[async_trait]
impl Matcher for BlobfsMatcher {
    async fn match_device(&self, device: &mut dyn Device) -> bool {
        self.0.match_device(device).await
    }

    async fn process_device(
        &mut self,
        device: &mut dyn Device,
        env: &mut dyn Environment,
    ) -> Result<(), Error> {
        env.mount_blobfs(device).await
    }
}

// Matches against a Data partition (by checking for partition label and type GUID).
struct DataMatcher(PartitionMatcher, PartitionMatcher);

impl DataMatcher {
    fn new() -> Self {
        Self(
            PartitionMatcher::new(DATA_PARTITION_LABEL, &DATA_TYPE_GUID),
            PartitionMatcher::new(LEGACY_DATA_PARTITION_LABEL, &DATA_TYPE_GUID),
        )
    }
}

#[async_trait]
impl Matcher for DataMatcher {
    async fn match_device(&self, device: &mut dyn Device) -> bool {
        self.0.match_device(device).await || self.1.match_device(device).await
    }

    async fn process_device(
        &mut self,
        device: &mut dyn Device,
        env: &mut dyn Environment,
    ) -> Result<(), Error> {
        let fs = match env.launch_data(device).await? {
            ServeFilesystemStatus::Serving(fs) => fs,
            ServeFilesystemStatus::FormatRequired => env.format_data(device).await?,
        };
        env.bind_data(fs)
    }
}

#[cfg(test)]
mod tests {
    use {
        super::{Device, DiskFormat, Environment, Matchers},
        crate::{
            config::default_config,
            device::constants::{
                BLOBFS_PARTITION_LABEL, BLOBFS_TYPE_GUID, BOOTPART_DRIVER_PATH,
                DATA_PARTITION_LABEL, DATA_TYPE_GUID, FVM_DRIVER_PATH, GPT_DRIVER_PATH,
                LEGACY_DATA_PARTITION_LABEL, NAND_BROKER_DRIVER_PATH,
            },
            environment::{Filesystem, ServeFilesystemStatus},
        },
        anyhow::{anyhow, Error},
        async_trait::async_trait,
        fidl_fuchsia_device::ControllerProxy,
        fidl_fuchsia_hardware_block::{BlockInfo, BlockProxy, Flag},
        fidl_fuchsia_hardware_block_volume::VolumeProxy,
        std::sync::Mutex,
    };

    #[derive(Clone)]
    struct MockDevice {
        block_flags: Flag,
        is_nand: bool,
        content_format: DiskFormat,
        topological_path: String,
        partition_label: Option<String>,
        partition_type: Option<[u8; 16]>,
    }

    impl MockDevice {
        fn new() -> Self {
            MockDevice {
                block_flags: Flag::empty(),
                is_nand: false,
                content_format: DiskFormat::Unknown,
                topological_path: "mock_device".to_string(),
                partition_label: None,
                partition_type: None,
            }
        }
        fn set_block_flags(mut self, flags: Flag) -> Self {
            self.block_flags = flags;
            self
        }
        fn set_nand(mut self, v: bool) -> Self {
            self.is_nand = v;
            self
        }
        fn set_content_format(mut self, format: DiskFormat) -> Self {
            self.content_format = format;
            self
        }
        fn set_topological_path(mut self, path: impl ToString) -> Self {
            self.topological_path = path.to_string().into();
            self
        }
        fn set_partition_label(mut self, label: impl ToString) -> Self {
            self.partition_label = Some(label.to_string());
            self
        }
        fn set_partition_type(mut self, partition_type: &[u8; 16]) -> Self {
            self.partition_type = Some(partition_type.clone());
            self
        }
    }

    #[async_trait]
    impl Device for MockDevice {
        async fn get_block_info(&self) -> Result<fidl_fuchsia_hardware_block::BlockInfo, Error> {
            if self.is_nand {
                Err(anyhow!("not supported by nand device"))
            } else {
                Ok(BlockInfo {
                    block_count: 0,
                    block_size: 0,
                    max_transfer_size: 0,
                    flags: self.block_flags,
                })
            }
        }
        fn is_nand(&self) -> bool {
            self.is_nand
        }
        async fn content_format(&mut self) -> Result<DiskFormat, Error> {
            Ok(self.content_format)
        }
        fn topological_path(&self) -> &str {
            &self.topological_path
        }
        fn path(&self) -> &str {
            &self.topological_path
        }
        async fn partition_label(&mut self) -> Result<&str, Error> {
            Ok(self
                .partition_label
                .as_ref()
                .unwrap_or_else(|| panic!("Unexpected call to partition_label")))
        }
        async fn partition_type(&mut self) -> Result<&[u8; 16], Error> {
            Ok(self
                .partition_type
                .as_ref()
                .unwrap_or_else(|| panic!("Unexpected call to partition_type")))
        }
        async fn partition_instance(&mut self) -> Result<&[u8; 16], Error> {
            unreachable!()
        }
        fn controller(&self) -> &ControllerProxy {
            unreachable!()
        }
        fn reopen_controller(&self) -> Result<ControllerProxy, Error> {
            unreachable!()
        }
        fn block_proxy(&self) -> Result<BlockProxy, Error> {
            unreachable!()
        }
        fn volume_proxy(&self) -> Result<VolumeProxy, Error> {
            unreachable!()
        }
        async fn get_child(&self, _suffix: &str) -> Result<Box<dyn Device>, Error> {
            unreachable!()
        }
    }

    struct MockEnv {
        expected_driver_path: Mutex<Option<String>>,
        expect_mount_blobfs: Mutex<bool>,
        expect_mount_fxblob: Mutex<bool>,
        expect_mount_blob_volume: Mutex<bool>,
        expect_mount_data_volume: Mutex<bool>,
        expect_format_data: Mutex<bool>,
        expect_bind_data: Mutex<bool>,
        data_format_required: bool,
    }

    impl MockEnv {
        fn new() -> Self {
            MockEnv {
                expected_driver_path: Mutex::new(None),
                expect_mount_blobfs: Mutex::new(false),
                expect_mount_fxblob: Mutex::new(false),
                expect_mount_blob_volume: Mutex::new(false),
                expect_mount_data_volume: Mutex::new(false),
                expect_format_data: Mutex::new(false),
                expect_bind_data: Mutex::new(false),
                data_format_required: false,
            }
        }
        fn expect_attach_driver(mut self, path: impl ToString) -> Self {
            *self.expected_driver_path.get_mut().unwrap() = Some(path.to_string());
            self
        }
        fn expect_mount_blobfs(mut self) -> Self {
            *self.expect_mount_blobfs.get_mut().unwrap() = true;
            self
        }
        fn expect_format_data(mut self) -> Self {
            *self.expect_format_data.get_mut().unwrap() = true;
            self
        }
        fn expect_bind_data(mut self) -> Self {
            *self.expect_bind_data.get_mut().unwrap() = true;
            self
        }
        fn expect_mount_fxblob(mut self) -> Self {
            *self.expect_mount_fxblob.get_mut().unwrap() = true;
            self
        }
        fn expect_mount_blob_volume(mut self) -> Self {
            *self.expect_mount_blob_volume.get_mut().unwrap() = true;
            self
        }
        fn expect_mount_data_volume(mut self) -> Self {
            *self.expect_mount_data_volume.get_mut().unwrap() = true;
            self
        }
        fn data_format_required(mut self) -> Self {
            self.data_format_required = true;
            self
        }
    }

    #[async_trait]
    impl Environment for MockEnv {
        async fn attach_driver(
            &self,
            _device: &mut dyn Device,
            driver_path: &str,
        ) -> Result<(), Error> {
            assert_eq!(
                driver_path,
                self.expected_driver_path
                    .lock()
                    .unwrap()
                    .take()
                    .expect("Unexpected call to attach_driver")
            );
            Ok(())
        }

        async fn mount_blobfs(&mut self, _device: &mut dyn Device) -> Result<(), Error> {
            assert_eq!(
                std::mem::take(&mut *self.expect_mount_blobfs.lock().unwrap()),
                true,
                "Unexpected call to mount_blobfs"
            );
            Ok(())
        }

        async fn launch_data(
            &mut self,
            _device: &mut dyn Device,
        ) -> Result<ServeFilesystemStatus, Error> {
            Ok(if self.data_format_required {
                ServeFilesystemStatus::FormatRequired
            } else {
                ServeFilesystemStatus::Serving(Filesystem::Queue(vec![]))
            })
        }

        async fn format_data(&mut self, _device: &mut dyn Device) -> Result<Filesystem, Error> {
            assert_eq!(
                std::mem::take(&mut *self.expect_format_data.lock().unwrap()),
                true,
                "Unexpected call to format_data"
            );
            Ok(Filesystem::Queue(vec![]))
        }

        fn bind_data(&mut self, mut _fs: Filesystem) -> Result<(), Error> {
            assert_eq!(
                std::mem::take(&mut *self.expect_bind_data.lock().unwrap()),
                true,
                "Unexpected call to bind_data"
            );
            Ok(())
        }

        async fn mount_fxblob(&mut self, _device: &mut dyn Device) -> Result<(), Error> {
            assert_eq!(
                std::mem::take(&mut *self.expect_mount_fxblob.lock().unwrap()),
                true,
                "Unexpected call to mount_fxblob"
            );
            Ok(())
        }

        async fn mount_blob_volume(&mut self) -> Result<(), Error> {
            assert_eq!(
                std::mem::take(&mut *self.expect_mount_blob_volume.lock().unwrap()),
                true,
                "Unexpected call to mount_blob_volume"
            );
            Ok(())
        }

        async fn mount_data_volume(&mut self) -> Result<(), Error> {
            assert_eq!(
                std::mem::take(&mut *self.expect_mount_data_volume.lock().unwrap()),
                true,
                "Unexpected call to mount_data_volume"
            );
            Ok(())
        }
    }

    impl Drop for MockEnv {
        fn drop(&mut self) {
            assert!(self.expected_driver_path.get_mut().unwrap().is_none());
            assert!(!*self.expect_mount_blobfs.lock().unwrap());
            assert!(!*self.expect_bind_data.lock().unwrap());
        }
    }

    #[fuchsia::test]
    async fn test_bootpart_matcher() {
        let mut mock_device = MockDevice::new().set_block_flags(Flag::BOOTPART);

        // Check no match when disabled in config.
        assert!(!Matchers::new(
            &fshost_config::Config { bootpart: false, ..default_config() },
            None
        )
        .match_device(&mut mock_device, &mut MockEnv::new())
        .await
        .expect("match_device failed"));

        assert!(Matchers::new(&default_config(), None)
            .match_device(
                &mut mock_device,
                &mut MockEnv::new().expect_attach_driver(BOOTPART_DRIVER_PATH)
            )
            .await
            .expect("match_device failed"));
    }

    #[fuchsia::test]
    async fn test_nand_matcher() {
        let mut device = MockDevice::new().set_nand(true);
        let mut env = MockEnv::new().expect_attach_driver(NAND_BROKER_DRIVER_PATH);

        // Default shouldn't match.
        assert!(!Matchers::new(&default_config(), None)
            .match_device(&mut device, &mut env)
            .await
            .expect("match_device failed"));

        assert!(Matchers::new(&fshost_config::Config { nand: true, ..default_config() }, None)
            .match_device(&mut device, &mut env)
            .await
            .expect("match_device failed"));
    }

    #[fuchsia::test]
    async fn test_partition_map_matcher() {
        let mut env = MockEnv::new().expect_attach_driver(GPT_DRIVER_PATH);

        // Check no match when disabled in config.
        let mut device = MockDevice::new().set_content_format(DiskFormat::Gpt);
        assert!(!Matchers::new(
            &fshost_config::Config { blobfs: false, data: false, gpt: false, ..default_config() },
            None
        )
        .match_device(&mut device, &mut env)
        .await
        .expect("match_device failed"));

        let mut matchers = Matchers::new(&default_config(), None);
        assert!(matchers.match_device(&mut device, &mut env).await.expect("match_device failed"));

        // More GPT devices should not get matched.
        assert!(!matchers.match_device(&mut device, &mut env).await.expect("match_device failed"));

        // The gpt_all config should allow multiple GPT devices to be matched.
        let mut matchers =
            Matchers::new(&fshost_config::Config { gpt_all: true, ..default_config() }, None);
        let mut env = MockEnv::new().expect_attach_driver(GPT_DRIVER_PATH);
        assert!(matchers.match_device(&mut device, &mut env).await.expect("match_device failed"));
        let mut env = MockEnv::new().expect_attach_driver(GPT_DRIVER_PATH);
        assert!(matchers.match_device(&mut device, &mut env).await.expect("match_device failed"));
    }

    #[fuchsia::test]
    async fn test_partition_map_matcher_ramdisk_prefix() {
        // If ramdisk_image is true and one of the devices matches the ramdisk prefix, we will match
        // two fvm devices, and the third one will fail.
        let mut matchers = Matchers::new(
            &fshost_config::Config {
                ramdisk_image: true,
                data_filesystem_format: "minfs".to_string(),
                ..default_config()
            },
            Some("second_prefix".to_string()),
        );
        let mut fvm_device = MockDevice::new()
            .set_content_format(DiskFormat::Fvm)
            .set_topological_path("first_prefix");
        let mut env = MockEnv::new().expect_attach_driver(FVM_DRIVER_PATH);
        assert!(matchers
            .match_device(&mut fvm_device, &mut env)
            .await
            .expect("match_device failed"));

        let mut fvm_device = fvm_device.set_topological_path("second_prefix");
        let mut env = MockEnv::new().expect_attach_driver(FVM_DRIVER_PATH);
        assert!(matchers
            .match_device(&mut fvm_device, &mut env)
            .await
            .expect("match_device failed"));

        let mut fvm_device = fvm_device.set_topological_path("third_prefix");
        assert!(!matchers
            .match_device(&mut fvm_device, &mut MockEnv::new())
            .await
            .expect("match_device failed"));

        // Afterwards, filesystems will work on the device with the right prefix, and not on other
        // devices without that prefix.
        let mut blobfs_device = MockDevice::new()
            .set_topological_path("first_prefix/fvm/blobfs-p-1/block")
            .set_partition_label(BLOBFS_PARTITION_LABEL)
            .set_partition_type(&BLOBFS_TYPE_GUID);
        assert!(!matchers
            .match_device(&mut blobfs_device, &mut MockEnv::new())
            .await
            .expect("match_device failed"));
        let mut blobfs_device =
            blobfs_device.set_topological_path("second_prefix/fvm/blobfs-p-1/block");
        let mut env = MockEnv::new().expect_mount_blobfs();
        assert!(matchers
            .match_device(&mut blobfs_device, &mut env)
            .await
            .expect("match_device failed"));

        // We will NOT bind to the device which matched the non-ramdisk prefix.
        let mut data_device = MockDevice::new()
            .set_topological_path("first_prefix/fvm/data-p-2/block")
            .set_partition_label(DATA_PARTITION_LABEL)
            .set_partition_type(&DATA_TYPE_GUID);
        let mut env = MockEnv::new();
        assert!(!matchers
            .match_device(&mut data_device, &mut env)
            .await
            .expect("match_device failed"));
    }

    #[fuchsia::test]
    async fn partition_map_matcher_wrong_prefix_match() {
        // If ramdisk_image is true but no devices match the prefix, only the first device will
        // match.
        let mut matchers = Matchers::new(
            &fshost_config::Config {
                ramdisk_image: true,
                data_filesystem_format: "fxfs".to_string(),
                ..default_config()
            },
            Some("wrong_prefix".to_string()),
        );

        let mut fvm_device = MockDevice::new()
            .set_content_format(DiskFormat::Fvm)
            .set_topological_path("first_prefix");
        let mut env = MockEnv::new().expect_attach_driver(FVM_DRIVER_PATH);
        assert!(matchers
            .match_device(&mut fvm_device, &mut env)
            .await
            .expect("match_device failed"));
        let mut fvm_device = fvm_device.set_topological_path("second_prefix");
        assert!(!matchers
            .match_device(&mut fvm_device, &mut MockEnv::new())
            .await
            .expect("match_device failed"));

        // Does not match due to the wrong prefix.
        let mut data_device = MockDevice::new()
            .set_topological_path("first_prefix/fvm/data-p-2/block")
            .set_partition_label(DATA_PARTITION_LABEL)
            .set_partition_type(&DATA_TYPE_GUID);
        assert!(!matchers
            .match_device(&mut data_device, &mut MockEnv::new())
            .await
            .expect("match_device failed"));
    }

    #[fuchsia::test]
    async fn partition_map_matcher_no_prefix_match() {
        // If ramdisk_image is true but no ramdisk path is provided, only the first device will
        // match.
        let mut matchers = Matchers::new(
            &fshost_config::Config {
                ramdisk_image: true,
                data_filesystem_format: "fxfs".to_string(),
                ..default_config()
            },
            None,
        );

        let mut fvm_device = MockDevice::new()
            .set_content_format(DiskFormat::Fvm)
            .set_topological_path("first_prefix");
        let mut env = MockEnv::new().expect_attach_driver(FVM_DRIVER_PATH);
        assert!(matchers
            .match_device(&mut fvm_device, &mut env)
            .await
            .expect("match_device failed"));
        let mut fvm_device = fvm_device.set_topological_path("second_prefix");
        assert!(!matchers
            .match_device(&mut fvm_device, &mut MockEnv::new())
            .await
            .expect("match_device failed"));
    }

    #[fuchsia::test]
    async fn test_blobfs_matcher() {
        fn fake_blobfs_device() -> MockDevice {
            MockDevice::new()
                .set_topological_path("mock_device/fvm/blobfs-p-1/block")
                .set_partition_label(BLOBFS_PARTITION_LABEL)
                .set_partition_type(&BLOBFS_TYPE_GUID)
        }

        let mut fvm_device = MockDevice::new().set_content_format(DiskFormat::Fvm);
        let mut env = MockEnv::new().expect_attach_driver(FVM_DRIVER_PATH).expect_mount_blobfs();

        let mut matchers = Matchers::new(&default_config(), None);

        // Attach the first GPT device.
        assert!(matchers
            .match_device(&mut fvm_device, &mut env)
            .await
            .expect("match_device failed"));

        // Attaching blobfs with a different path should fail.
        assert!(!matchers
            .match_device(
                &mut fake_blobfs_device()
                    .set_topological_path("another_device/fvm/blobfs-p-1/block"),
                &mut env
            )
            .await
            .expect("match_device failed"));

        // Attaching blobfs with a different label should fail.
        assert!(!matchers
            .match_device(&mut fake_blobfs_device().set_partition_label("data"), &mut env)
            .await
            .expect("match_device failed"));

        // Attaching blobfs with a different type should fail.
        assert!(!matchers
            .match_device(&mut fake_blobfs_device().set_partition_type(&[1; 16]), &mut env)
            .await
            .expect("match_device failed"));

        // Attach blobfs.
        assert!(matchers
            .match_device(&mut fake_blobfs_device(), &mut env)
            .await
            .expect("match_device failed"));
    }

    #[fuchsia::test]
    async fn test_data_matcher() {
        let mut matchers = Matchers::new(&default_config(), None);

        // Attach FVM device.
        assert!(matchers
            .match_device(
                &mut MockDevice::new().set_content_format(DiskFormat::Fvm),
                &mut MockEnv::new().expect_attach_driver(FVM_DRIVER_PATH)
            )
            .await
            .expect("match_device failed"));

        // Check that the data partition is mounted.
        assert!(matchers
            .match_device(
                &mut MockDevice::new()
                    .set_topological_path("mock_device/fvm/data-p-2/block")
                    .set_partition_label(DATA_PARTITION_LABEL)
                    .set_partition_type(&DATA_TYPE_GUID),
                &mut MockEnv::new().expect_bind_data()
            )
            .await
            .expect("match_device failed"));
    }

    #[fuchsia::test]
    async fn test_legacy_data_matcher() {
        let mut matchers = Matchers::new(&default_config(), None);

        // Attach FVM device.
        assert!(matchers
            .match_device(
                &mut MockDevice::new().set_content_format(DiskFormat::Fvm),
                &mut MockEnv::new().expect_attach_driver(FVM_DRIVER_PATH)
            )
            .await
            .expect("match_device failed"));

        // Check that the data partition is mounted with the legacy label.
        assert!(matchers
            .match_device(
                &mut MockDevice::new()
                    .set_topological_path("mock_device/fvm/data-p-2/block")
                    .set_partition_label(LEGACY_DATA_PARTITION_LABEL)
                    .set_partition_type(&DATA_TYPE_GUID),
                &mut MockEnv::new().expect_bind_data()
            )
            .await
            .expect("match_device failed"));
    }

    #[fuchsia::test]
    async fn test_data_matcher_reformat() {
        let mut matchers = Matchers::new(&default_config(), None);

        // Attach FVM device.
        assert!(matchers
            .match_device(
                &mut MockDevice::new().set_content_format(DiskFormat::Fvm),
                &mut MockEnv::new().expect_attach_driver(FVM_DRIVER_PATH)
            )
            .await
            .expect("match_device failed"));

        // Check that the data partition is reformatted and then mounted.
        assert!(matchers
            .match_device(
                &mut MockDevice::new()
                    .set_topological_path("mock_device/fvm/data-p-2/block")
                    .set_partition_label(DATA_PARTITION_LABEL)
                    .set_partition_type(&DATA_TYPE_GUID),
                &mut MockEnv::new().data_format_required().expect_format_data().expect_bind_data()
            )
            .await
            .expect("match_device failed"));
    }

    #[fuchsia::test]
    async fn test_netboot_flag_true() {
        let mut matchers =
            Matchers::new(&fshost_config::Config { netboot: true, ..default_config() }, None);

        // Attach FVM device.
        assert!(matchers
            .match_device(
                &mut MockDevice::new().set_content_format(DiskFormat::Fvm),
                &mut MockEnv::new().expect_attach_driver(FVM_DRIVER_PATH)
            )
            .await
            .expect("match_device failed"));

        let env = &mut MockEnv::new();

        // Attaching blobfs should fail if netboot is true
        assert!(!matchers
            .match_device(
                &mut MockDevice::new()
                    .set_topological_path("mock_device/fvm/blobfs-p-1/block")
                    .set_partition_label(DATA_PARTITION_LABEL)
                    .set_partition_type(&DATA_TYPE_GUID),
                env
            )
            .await
            .expect("match_device failed"));

        // Attaching data should fail if netboot is true
        assert!(!matchers
            .match_device(
                &mut MockDevice::new()
                    .set_topological_path("mock_device/fvm/data-p-2/block")
                    .set_partition_label(DATA_PARTITION_LABEL)
                    .set_partition_type(&DATA_TYPE_GUID),
                env
            )
            .await
            .expect("match_device failed"));
    }

    #[fuchsia::test]
    async fn test_fxblob_matcher() {
        let mut matchers = Matchers::new(
            &fshost_config::Config {
                fxfs_blob: true,
                data_filesystem_format: "fxfs".to_string(),
                ..default_config()
            },
            None,
        );
        // Check that the data partition is mounted.
        assert!(matchers
            .match_device(
                &mut MockDevice::new()
                    .set_content_format(DiskFormat::Fxfs)
                    .set_topological_path("mock_device/data-p-2/block")
                    .set_partition_label(DATA_PARTITION_LABEL)
                    .set_partition_type(&DATA_TYPE_GUID),
                &mut MockEnv::new()
                    .expect_mount_fxblob()
                    .expect_mount_blob_volume()
                    .expect_mount_data_volume()
            )
            .await
            .expect("match_device failed"));
    }
}
