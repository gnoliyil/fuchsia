// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    async_trait::async_trait,
    either::Either,
    fidl_fuchsia_fs::AdminMarker,
    fidl_fuchsia_fs_startup::{
        CompressionAlgorithm, EvictionPolicyOverride, StartOptions, StartupMarker,
    },
    fidl_fuchsia_fxfs::{CryptManagementMarker, CryptMarker, KeyPurpose, MountOptions},
    fidl_fuchsia_hardware_block::BlockMarker,
    fidl_fuchsia_io as fio,
    fs_management::{
        filesystem::{ServingMultiVolumeFilesystem, ServingSingleVolumeFilesystem},
        FSConfig,
    },
    fuchsia_component::client::{
        connect_channel_to_protocol, connect_to_childs_protocol, open_childs_exposed_directory,
    },
    fuchsia_zircon as zx,
    std::{
        path::Path,
        sync::{Arc, Once},
    },
    storage_benchmarks::{
        block_device::BlockDevice, BlockDeviceConfig, BlockDeviceFactory, Filesystem,
        FilesystemConfig,
    },
};

const MOUNT_PATH: &str = "/benchmark";

struct FsmFilesystem {
    fs: fs_management::filesystem::Filesystem,
    serving_filesystem: Option<Either<ServingSingleVolumeFilesystem, ServingMultiVolumeFilesystem>>,
    as_blob: bool,
    // Keep the underlying block device alive for as long as we are using the filesystem.
    _block_device: Box<dyn BlockDevice>,
}

impl FsmFilesystem {
    pub async fn new<FSC: FSConfig>(
        config: FSC,
        mut block_device: Box<dyn BlockDevice>,
        as_blob: bool,
    ) -> Self {
        let controller = block_device.take_controller().expect("invalid device controller");
        let mut fs = fs_management::filesystem::Filesystem::new(controller, config);
        fs.format().await.expect("Failed to format the filesystem");
        let serving_filesystem = if fs.config().is_multi_volume() {
            let mut serving_filesystem =
                fs.serve_multi_volume().await.expect("Failed to start the filesystem");
            let vol = serving_filesystem
                .create_volume(
                    "default",
                    MountOptions { crypt: fs.config().crypt_client().map(|c| c.into()), as_blob },
                )
                .await
                .expect("Failed to create volume");
            vol.bind_to_path(MOUNT_PATH).expect("Failed to bind the volume");
            Either::Right(serving_filesystem)
        } else {
            let mut serving_filesystem = fs.serve().await.expect("Failed to start the filesystem");
            serving_filesystem.bind_to_path(MOUNT_PATH).expect("Failed to bind the filesystem");
            Either::Left(serving_filesystem)
        };
        Self {
            fs,
            serving_filesystem: Some(serving_filesystem),
            _block_device: block_device,
            as_blob,
        }
    }
}

#[async_trait]
impl Filesystem for FsmFilesystem {
    async fn clear_cache(&mut self) {
        // Remount the filesystem to guarantee that all cached data from reads and write is cleared.
        let serving_filesystem = self.serving_filesystem.take().unwrap();
        let serving_filesystem = match serving_filesystem {
            Either::Left(serving_filesystem) => {
                serving_filesystem.shutdown().await.expect("Failed to stop the filesystem");
                let mut serving_filesystem =
                    self.fs.serve().await.expect("Failed to start the filesystem");
                serving_filesystem.bind_to_path(MOUNT_PATH).expect("Failed to bind the filesystem");
                Either::Left(serving_filesystem)
            }
            Either::Right(serving_filesystem) => {
                serving_filesystem.shutdown().await.expect("Failed to stop the filesystem");
                let mut serving_filesystem =
                    self.fs.serve_multi_volume().await.expect("Failed to start the filesystem");
                let vol = serving_filesystem
                    .open_volume(
                        "default",
                        MountOptions {
                            crypt: self.fs.config().crypt_client().map(|c| c.into()),
                            as_blob: self.as_blob,
                        },
                    )
                    .await
                    .expect("Failed to create volume");
                vol.bind_to_path(MOUNT_PATH).expect("Failed to bind the volume");
                Either::Right(serving_filesystem)
            }
        };
        self.serving_filesystem = Some(serving_filesystem);
    }

    async fn shutdown(mut self: Box<Self>) {
        if let Some(fs) = self.serving_filesystem {
            match fs {
                Either::Left(fs) => fs.shutdown().await.expect("Failed to stop filesystem"),
                Either::Right(fs) => fs.shutdown().await.expect("Failed to stop filesystem"),
            }
        }
    }

    fn benchmark_dir(&self) -> &Path {
        Path::new(MOUNT_PATH)
    }

    fn exposed_dir(&mut self) -> &fio::DirectoryProxy {
        let fs = self.serving_filesystem.as_ref().unwrap();
        match fs {
            Either::Left(serving_filesystem) => serving_filesystem.exposed_dir(),
            Either::Right(serving_filesystem) => {
                serving_filesystem.volume("default").unwrap().exposed_dir()
            }
        }
    }
}

pub struct Blobfs {}

impl Blobfs {
    pub fn new() -> Self {
        Self {}
    }
}

#[async_trait]
impl FilesystemConfig for Blobfs {
    async fn start_filesystem(
        &self,
        block_device_factory: &dyn BlockDeviceFactory,
    ) -> Box<dyn Filesystem> {
        let block_device = block_device_factory
            .create_block_device(&BlockDeviceConfig { use_zxcrypt: false, fvm_volume_size: None })
            .await;
        Box::new(
            FsmFilesystem::new(
                fs_management::Blobfs::default(),
                block_device,
                /*as_blob=*/ false,
            )
            .await,
        )
    }

    fn name(&self) -> String {
        "blobfs".to_owned()
    }
}

fn get_crypt_client() -> zx::Channel {
    static CRYPT_CLIENT_INITIALIZER: Once = Once::new();
    CRYPT_CLIENT_INITIALIZER.call_once(|| {
        let (client_end, server_end) = zx::Channel::create();
        connect_channel_to_protocol::<CryptManagementMarker>(server_end)
            .expect("Failed to connect to the crypt management service");
        let crypt_management_service =
            fidl_fuchsia_fxfs::CryptManagementSynchronousProxy::new(client_end);

        let mut key = [0; 32];
        zx::cprng_draw(&mut key);
        match crypt_management_service
            .add_wrapping_key(0, &key, zx::Time::INFINITE)
            .expect("FIDL failed")
            .map_err(zx::Status::from_raw)
        {
            Ok(()) => {}
            Err(zx::Status::ALREADY_EXISTS) => {
                // In tests, the binary is run multiple times which gets around the `Once`. The fxfs
                // crypt component is not restarted for each test so it may already be initialized.
                return;
            }
            Err(e) => panic!("add_wrapping_key failed: {:?}", e),
        };
        zx::cprng_draw(&mut key);
        crypt_management_service
            .add_wrapping_key(1, &key, zx::Time::INFINITE)
            .expect("FIDL failed")
            .map_err(zx::Status::from_raw)
            .expect("add_wrapping_key failed");
        crypt_management_service
            .set_active_key(KeyPurpose::Data, 0, zx::Time::INFINITE)
            .expect("FIDL failed")
            .map_err(zx::Status::from_raw)
            .expect("set_active_key failed");
        crypt_management_service
            .set_active_key(KeyPurpose::Metadata, 1, zx::Time::INFINITE)
            .expect("FIDL failed")
            .map_err(zx::Status::from_raw)
            .expect("set_active_key failed");
    });
    let (client_end, server_end) = zx::Channel::create();
    connect_channel_to_protocol::<CryptMarker>(server_end)
        .expect("Failed to connect to crypt service");
    client_end
}

pub struct Fxfs {}

impl Fxfs {
    pub fn new() -> Self {
        Self {}
    }
}

#[async_trait]
impl FilesystemConfig for Fxfs {
    async fn start_filesystem(
        &self,
        block_device_factory: &dyn BlockDeviceFactory,
    ) -> Box<dyn Filesystem> {
        let block_device = block_device_factory
            .create_block_device(&BlockDeviceConfig {
                use_zxcrypt: false,
                fvm_volume_size: Some(60 * 1024 * 1024),
            })
            .await;
        let fxfs = FsmFilesystem::new(
            fs_management::Fxfs::with_crypt_client(Arc::new(get_crypt_client)),
            block_device,
            /*as_blob=*/ false,
        )
        .await;
        Box::new(fxfs)
    }

    fn name(&self) -> String {
        "fxfs".to_owned()
    }
}

pub struct Fxblob {}

impl Fxblob {
    pub fn new() -> Self {
        Self {}
    }
}

#[async_trait]
impl FilesystemConfig for Fxblob {
    async fn start_filesystem(
        &self,
        block_device_factory: &dyn BlockDeviceFactory,
    ) -> Box<dyn Filesystem> {
        let block_device = block_device_factory
            .create_block_device(&BlockDeviceConfig {
                use_zxcrypt: false,
                fvm_volume_size: Some(125 * 1024 * 1024),
            })
            .await;
        let fxblob = FsmFilesystem::new(
            fs_management::Fxfs::default(),
            block_device,
            /*as_blob=*/ true,
        )
        .await;
        Box::new(fxblob)
    }

    fn name(&self) -> String {
        "fxblob".to_owned()
    }
}

pub struct F2fs {}

impl F2fs {
    pub fn new() -> Self {
        Self {}
    }
}

#[async_trait]
impl FilesystemConfig for F2fs {
    async fn start_filesystem(
        &self,
        block_device_factory: &dyn BlockDeviceFactory,
    ) -> Box<dyn Filesystem> {
        let block_device = block_device_factory
            .create_block_device(&BlockDeviceConfig {
                use_zxcrypt: true,
                // f2fs requires a minimum of 100mb volume for fsync test
                fvm_volume_size: Some(100 * 1024 * 1024),
            })
            .await;
        Box::new(
            FsmFilesystem::new(
                fs_management::F2fs::default(),
                block_device,
                /*as_blob=*/ false,
            )
            .await,
        )
    }

    fn name(&self) -> String {
        "f2fs".to_owned()
    }
}

struct MemfsInstance {
    exposed_dir: fio::DirectoryProxy,
}

impl MemfsInstance {
    pub async fn new() -> Self {
        let startup = connect_to_childs_protocol::<StartupMarker>("memfs".to_string(), None)
            .await
            .expect("Failed to connect to memfs");
        let options = StartOptions {
            read_only: false,
            verbose: false,
            fsck_after_every_transaction: false,
            write_compression_algorithm: CompressionAlgorithm::ZstdChunked,
            write_compression_level: 0,
            cache_eviction_policy_override: EvictionPolicyOverride::None,
            allow_delivery_blobs: false,
        };
        // Memfs doesn't need a block device but FIDL prevents passing an invalid handle.
        let (device_client_end, _) = fidl::endpoints::create_endpoints::<BlockMarker>();
        startup
            .start(device_client_end, options)
            .await
            .unwrap()
            .map_err(zx::Status::from_raw)
            .unwrap();

        let exposed_dir = open_childs_exposed_directory("memfs", None)
            .await
            .expect("Failed to connect to memfs's exposed directory");

        let (root_dir, server_end) = fidl::endpoints::create_endpoints();
        exposed_dir
            .open(
                fio::OpenFlags::RIGHT_READABLE
                    | fio::OpenFlags::POSIX_EXECUTABLE
                    | fio::OpenFlags::POSIX_WRITABLE,
                fio::ModeType::empty(),
                "root",
                fidl::endpoints::ServerEnd::new(server_end.into_channel()),
            )
            .expect("Failed to open memfs's root");
        let namespace = fdio::Namespace::installed().expect("Failed to get local namespace");
        namespace.bind(MOUNT_PATH, root_dir).expect("Failed to bind memfs");

        Self { exposed_dir }
    }
}

#[async_trait]
impl Filesystem for MemfsInstance {
    async fn clear_cache(&mut self) {}

    async fn shutdown(mut self: Box<Self>) {
        let admin = connect_to_childs_protocol::<AdminMarker>("memfs".to_string(), None)
            .await
            .expect("Failed to connect to memfs Admin");
        admin.shutdown().await.expect("Failed to shutdown memfs");

        let namespace = fdio::Namespace::installed().expect("Failed to get local namespace");
        namespace.unbind(MOUNT_PATH).expect("Failed to unbind memfs");
    }

    fn benchmark_dir(&self) -> &Path {
        Path::new(MOUNT_PATH)
    }

    fn exposed_dir(&mut self) -> &fio::DirectoryProxy {
        &self.exposed_dir
    }
}

pub struct Memfs {}

impl Memfs {
    pub fn new() -> Self {
        Self {}
    }
}

#[async_trait]
impl FilesystemConfig for Memfs {
    async fn start_filesystem(
        &self,
        _block_device_factory: &dyn BlockDeviceFactory,
    ) -> Box<dyn Filesystem> {
        Box::new(MemfsInstance::new().await)
    }

    fn name(&self) -> String {
        "memfs".to_owned()
    }
}

pub struct Minfs {}

impl Minfs {
    pub fn new() -> Self {
        Self {}
    }
}

#[async_trait]
impl FilesystemConfig for Minfs {
    async fn start_filesystem(
        &self,
        block_device_factory: &dyn BlockDeviceFactory,
    ) -> Box<dyn Filesystem> {
        let block_device = block_device_factory
            .create_block_device(&BlockDeviceConfig { use_zxcrypt: true, fvm_volume_size: None })
            .await;
        Box::new(
            FsmFilesystem::new(
                fs_management::Minfs::default(),
                block_device,
                /*as_blob=*/ false,
            )
            .await,
        )
    }

    fn name(&self) -> String {
        "minfs".to_owned()
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::block_devices::RamdiskFactory,
        blob_writer::BlobWriter,
        fidl::endpoints::{create_proxy, ServerEnd},
        fuchsia_component::client::connect_to_protocol_at_dir_svc,
        fuchsia_merkle::MerkleTree,
        std::{
            fs::OpenOptions,
            io::{Read, Write},
        },
    };
    const DEVICE_SIZE: u64 = 128 * 1024 * 1024;
    const BLOCK_SIZE: u64 = 4 * 1024;
    const BLOCK_COUNT: u64 = DEVICE_SIZE / BLOCK_SIZE;

    async fn check_blob_filesystem(filesystem: &dyn FilesystemConfig) {
        const BLOB_NAME: &str = "bd905f783ceae4c5ba8319703d7505ab363733c2db04c52c8405603a02922b15";
        const BLOB_CONTENTS: &str = "blob-contents";

        let ramdisk_factory = RamdiskFactory::new(BLOCK_SIZE, BLOCK_COUNT).await;
        let mut fs = filesystem.start_filesystem(&ramdisk_factory).await;
        let blob_path = fs.benchmark_dir().join(BLOB_NAME);

        {
            let mut file = OpenOptions::new()
                .create_new(true)
                .write(true)
                .truncate(true)
                .open(&blob_path)
                .unwrap();
            file.set_len(BLOB_CONTENTS.len() as u64).unwrap();
            file.write_all(BLOB_CONTENTS.as_bytes()).unwrap();
        }
        fs.clear_cache().await;
        {
            let mut file = OpenOptions::new().read(true).open(&blob_path).unwrap();
            let mut buf = [0u8; BLOB_CONTENTS.len()];
            file.read_exact(&mut buf).unwrap();
            assert_eq!(std::str::from_utf8(&buf).unwrap(), BLOB_CONTENTS);
        }

        fs.shutdown().await;
    }

    async fn check_blob_filesystem_new_write_api(filesystem: &dyn FilesystemConfig) {
        const BLOB_CONTENTS: &str = "blob-contents";
        let merkle = MerkleTree::from_reader(BLOB_CONTENTS.as_bytes()).unwrap().root();

        let ramdisk_factory = RamdiskFactory::new(BLOCK_SIZE, BLOCK_COUNT).await;
        let mut fs = filesystem.start_filesystem(&ramdisk_factory).await;

        let blob_proxy = connect_to_protocol_at_dir_svc::<fidl_fuchsia_fxfs::BlobCreatorMarker>(
            fs.exposed_dir(),
        )
        .expect("failed to connect to the BlobCreator service");
        let writer_client_end = blob_proxy
            .create(&merkle.into(), false)
            .await
            .expect("transport error on BlobCreator.Create")
            .expect("failed to create blob");
        let writer = writer_client_end.into_proxy().unwrap();
        let mut blob_writer = BlobWriter::create(writer, BLOB_CONTENTS.len() as u64)
            .await
            .expect("failed to create BlobWriter");
        blob_writer.write(BLOB_CONTENTS.as_bytes()).await.unwrap();

        let root = fuchsia_fs::directory::open_directory_no_describe(
            fs.exposed_dir(),
            "root",
            fio::OpenFlags::RIGHT_READABLE
                | fio::OpenFlags::POSIX_EXECUTABLE
                | fio::OpenFlags::POSIX_WRITABLE,
        )
        .unwrap();
        let (blob, server_end) = create_proxy::<fio::FileMarker>().expect("create_proxy failed");
        root.open(
            fio::OpenFlags::RIGHT_READABLE,
            fio::ModeType::empty(),
            &merkle.to_string(),
            ServerEnd::new(server_end.into_channel()),
        )
        .expect("failed to open blob");
        let _: Vec<_> = blob.query().await.expect("failed to query blob");

        let mut read_data = Vec::new();
        loop {
            let chunk = blob
                .read(fio::MAX_TRANSFER_SIZE)
                .await
                .expect("FIDL call failed")
                .expect("read failed");
            let done = chunk.len() < fio::MAX_TRANSFER_SIZE as usize;
            read_data.extend(chunk);
            if done {
                break;
            }
        }
        assert_eq!(BLOB_CONTENTS.as_bytes(), read_data.as_slice());
        fs.shutdown().await;
    }

    async fn check_filesystem(filesystem: &dyn FilesystemConfig) {
        const FILE_CONTENTS: &str = "file-contents";

        let ramdisk_factory = RamdiskFactory::new(BLOCK_SIZE, BLOCK_COUNT).await;
        let mut fs = filesystem.start_filesystem(&ramdisk_factory).await;

        let file_path = fs.benchmark_dir().join("filename");
        {
            let mut file =
                OpenOptions::new().create_new(true).write(true).open(&file_path).unwrap();
            file.write_all(FILE_CONTENTS.as_bytes()).unwrap();
        }
        fs.clear_cache().await;
        {
            let mut file = OpenOptions::new().read(true).open(&file_path).unwrap();
            let mut buf = [0u8; FILE_CONTENTS.len()];
            file.read_exact(&mut buf).unwrap();
            assert_eq!(std::str::from_utf8(&buf).unwrap(), FILE_CONTENTS);
        }
        fs.shutdown().await;
    }

    #[fuchsia::test]
    async fn start_blobfs() {
        check_blob_filesystem(&Blobfs::new()).await;
    }

    #[fuchsia::test]
    async fn start_fxfs() {
        check_filesystem(&Fxfs::new()).await;
    }

    #[fuchsia::test]
    async fn start_fxblob_old() {
        check_blob_filesystem(&Fxblob::new()).await;
    }

    #[fuchsia::test]
    async fn start_fxblob_new() {
        check_blob_filesystem_new_write_api(&Fxblob::new()).await;
    }

    #[fuchsia::test]
    async fn start_f2fs() {
        check_filesystem(&F2fs::new()).await;
    }

    #[fuchsia::test]
    async fn start_minfs() {
        check_filesystem(&Minfs::new()).await;
    }

    #[fuchsia::test]
    async fn start_memfs() {
        check_filesystem(&Memfs::new()).await;
    }
}
