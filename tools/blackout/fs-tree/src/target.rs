// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{Context, Result},
    async_trait::async_trait,
    blackout_target::{
        static_tree::{DirectoryEntry, EntryDistribution},
        Test, TestServer,
    },
    fidl::endpoints::Proxy as _,
    fidl_fuchsia_device::ControllerProxy,
    fidl_fuchsia_fxfs::{CryptManagementMarker, CryptMarker, KeyPurpose, MountOptions},
    fidl_fuchsia_io as fio,
    fs_management::{
        filesystem::{Filesystem, ServingMultiVolumeFilesystem, ServingSingleVolumeFilesystem},
        CryptClientFn, Fxfs, Minfs,
    },
    fuchsia_component::client::connect_to_protocol,
    fuchsia_zircon as zx,
    rand::{rngs::StdRng, Rng, SeedableRng},
    std::sync::{
        atomic::{AtomicBool, Ordering},
        Arc,
    },
};

const DATA_FILESYSTEM_FORMAT: &'static str = std::env!("DATA_FILESYSTEM_FORMAT");

const DATA_KEY: [u8; 32] = [
    0xcf, 0x9e, 0x45, 0x2a, 0x22, 0xa5, 0x70, 0x31, 0x33, 0x3b, 0x4d, 0x6b, 0x6f, 0x78, 0x58, 0x29,
    0x04, 0x79, 0xc7, 0xd6, 0xa9, 0x4b, 0xce, 0x82, 0x04, 0x56, 0x5e, 0x82, 0xfc, 0xe7, 0x37, 0xa8,
];

const METADATA_KEY: [u8; 32] = [
    0x0f, 0x4d, 0xca, 0x6b, 0x35, 0x0e, 0x85, 0x6a, 0xb3, 0x8c, 0xdd, 0xe9, 0xda, 0x0e, 0xc8, 0x22,
    0x8e, 0xea, 0xd8, 0x05, 0xc4, 0xc9, 0x0b, 0xa8, 0xd8, 0x85, 0x87, 0x50, 0x75, 0x40, 0x1c, 0x4c,
];

async fn find_partition_helper(
    device_path: Option<String>,
    device_label: String,
) -> Result<ControllerProxy> {
    let device_dir = match device_path {
        Some(path) => {
            let device_dir =
                fuchsia_fs::directory::open_in_namespace(&path, fio::OpenFlags::empty())?;
            Some(device_dir)
        }
        None => None,
    };
    blackout_target::find_partition(&device_label, device_dir.as_ref()).await
}

#[derive(Copy, Clone)]
struct FsTree;

impl FsTree {
    async fn setup_crypt_service(&self) -> Result<()> {
        static INITIALIZED: AtomicBool = AtomicBool::new(false);
        if INITIALIZED.load(Ordering::SeqCst) {
            return Ok(());
        }
        let crypt_management = connect_to_protocol::<CryptManagementMarker>()?;
        crypt_management.add_wrapping_key(0, &DATA_KEY).await?.map_err(zx::Status::from_raw)?;
        crypt_management.add_wrapping_key(1, &METADATA_KEY).await?.map_err(zx::Status::from_raw)?;
        crypt_management
            .set_active_key(KeyPurpose::Data, 0)
            .await?
            .map_err(zx::Status::from_raw)?;
        crypt_management
            .set_active_key(KeyPurpose::Metadata, 1)
            .await?
            .map_err(zx::Status::from_raw)?;
        INITIALIZED.store(true, Ordering::SeqCst);
        Ok(())
    }

    async fn setup_fxfs(&self, controller: ControllerProxy) -> Result<()> {
        let mut fxfs = Fxfs::new(controller);
        fxfs.format().await?;
        let mut fs = fxfs.serve_multi_volume().await?;
        self.setup_crypt_service().await?;
        let crypt_service = Some(
            connect_to_protocol::<CryptMarker>()?.into_channel().unwrap().into_zx_channel().into(),
        );
        fs.create_volume("default", MountOptions { crypt: crypt_service, as_blob: false }).await?;
        Ok(())
    }

    async fn setup_minfs(&self, controller: ControllerProxy) -> Result<()> {
        let mut minfs = Minfs::new(controller);
        minfs.format().await.context("failed to format minfs")?;
        Ok(())
    }

    async fn serve_fxfs(&self, controller: ControllerProxy) -> Result<FsInstance> {
        let mut fxfs = Fxfs::new(controller);
        let mut fs = fxfs.serve_multi_volume().await?;
        self.setup_crypt_service().await?;
        let crypt_service = Some(
            connect_to_protocol::<CryptMarker>()?.into_channel().unwrap().into_zx_channel().into(),
        );
        let _ = fs
            .open_volume("default", MountOptions { crypt: crypt_service, as_blob: false })
            .await?;
        Ok(FsInstance::Fxfs(fs))
    }

    async fn serve_minfs(&self, controller: ControllerProxy) -> Result<FsInstance> {
        let mut minfs = Minfs::new(controller);
        let fs = minfs.serve().await?;
        Ok(FsInstance::Minfs(fs))
    }

    async fn verify_fxfs(&self, controller: ControllerProxy) -> Result<()> {
        self.setup_crypt_service().await?;
        let crypt_client_fn = Some(Arc::new(|| {
            connect_to_protocol::<CryptMarker>()
                .unwrap()
                .into_channel()
                .unwrap()
                .into_zx_channel()
                .into()
        }) as CryptClientFn);
        let mut fxfs = Filesystem::new(controller, Fxfs { crypt_client_fn, ..Default::default() });
        fxfs.fsck().await?;
        Ok(())
    }

    async fn verify_minfs(&self, controller: ControllerProxy) -> Result<()> {
        let mut minfs = Minfs::new(controller);
        minfs.fsck().await?;
        Ok(())
    }
}

enum FsInstance {
    Fxfs(ServingMultiVolumeFilesystem),
    Minfs(ServingSingleVolumeFilesystem),
}

impl FsInstance {
    fn root(&self) -> &fio::DirectoryProxy {
        match &self {
            FsInstance::Fxfs(fs) => {
                let vol = fs.volume("default").expect("failed to get default volume");
                vol.root()
            }
            FsInstance::Minfs(fs) => fs.root(),
        }
    }
}

#[async_trait]
impl Test for FsTree {
    async fn setup(
        self: Arc<Self>,
        device_label: String,
        device_path: Option<String>,
        _seed: u64,
    ) -> Result<()> {
        let device_dir = match device_path {
            Some(path) => {
                let device_dir =
                    fuchsia_fs::directory::open_in_namespace(&path, fio::OpenFlags::empty())?;
                Some(device_dir)
            }
            None => None,
        };
        let partition_controller =
            blackout_target::set_up_partition(&device_label, device_dir.as_ref(), true).await?;

        match DATA_FILESYSTEM_FORMAT {
            "fxfs" => self.setup_fxfs(partition_controller).await,
            "minfs" => self.setup_minfs(partition_controller).await,
            _ => panic!("Unsupported filesystem"),
        }
    }

    async fn test(
        self: Arc<Self>,
        device_label: String,
        device_path: Option<String>,
        seed: u64,
    ) -> Result<()> {
        let partition_controller = find_partition_helper(device_path, device_label).await?;
        let fs = match DATA_FILESYSTEM_FORMAT {
            "fxfs" => self.serve_fxfs(partition_controller).await?,
            "minfs" => self.serve_minfs(partition_controller).await?,
            _ => panic!("Unsupported filesystem"),
        };

        tracing::info!("generating load");
        let mut rng = StdRng::seed_from_u64(seed);
        loop {
            tracing::info!("generating tree");
            let dist = EntryDistribution::new(6);
            let tree: DirectoryEntry = rng.sample(&dist);
            tracing::info!("generated tree: {:?}", tree);
            let tree_name = tree.get_name();
            tracing::info!("writing tree");
            tree.write_tree_at(fs.root()).await.context("failed to write directory tree")?;
            // now try renaming the tree root
            let tree_name2 = format!("{}-renamed", tree_name);
            tracing::info!("moving tree");
            fuchsia_fs::directory::rename(fs.root(), &tree_name, &tree_name2)
                .await
                .context("failed to rename directory tree")?;
            // then try deleting the entire thing.
            tracing::info!("deleting tree");
            fuchsia_fs::directory::remove_dir_recursive(fs.root(), &tree_name2)
                .await
                .context("failed to delete directory tree")?;
        }
    }

    async fn verify(
        self: Arc<Self>,
        device_label: String,
        device_path: Option<String>,
        _seed: u64,
    ) -> Result<()> {
        let partition_controller = find_partition_helper(device_path, device_label).await?;
        match DATA_FILESYSTEM_FORMAT {
            "fxfs" => self.verify_fxfs(partition_controller).await,
            "minfs" => self.verify_minfs(partition_controller).await,
            _ => panic!("Unsupported filesystem"),
        }
    }
}

#[fuchsia::main]
async fn main() -> Result<()> {
    let server = TestServer::new(FsTree)?;
    server.serve().await;

    Ok(())
}
