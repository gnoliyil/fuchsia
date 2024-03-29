// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::fuchsia::{
        directory::FxDirectory, file::FxFile, fxblob::BlobDirectory, pager::PagerBacked,
        volume::FxVolumeAndRoot, volumes_directory::VolumesDirectory,
    },
    anyhow::Context,
    anyhow::Error,
    fidl::endpoints::{create_proxy, ServerEnd},
    fidl_fuchsia_io as fio,
    fuchsia_zircon::{self as zx, Status},
    fxfs::{
        filesystem::{FxFilesystem, FxFilesystemBuilder, OpenFxFilesystem},
        fsck::{errors::FsckIssue, fsck_volume_with_options, fsck_with_options, FsckOptions},
        object_store::volume::root_volume,
    },
    fxfs_insecure_crypto::InsecureCrypt,
    std::sync::{Arc, Weak},
    storage_device::{fake_device::FakeDevice, DeviceHolder},
    vfs::{path::Path, temp_clone::unblock},
};

struct State {
    filesystem: OpenFxFilesystem,
    volume: FxVolumeAndRoot,
    volume_out_dir: Option<fio::DirectoryProxy>,
    root: fio::DirectoryProxy,
    volumes_directory: Arc<VolumesDirectory>,
}

pub struct TestFixture {
    state: Option<State>,
    encrypted: bool,
}

pub struct TestFixtureOptions {
    pub encrypted: bool,
    pub as_blob: bool,
    pub format: bool,
    pub serve_volume: bool,
}

impl TestFixture {
    pub async fn new() -> Self {
        Self::open(
            DeviceHolder::new(FakeDevice::new(16384, 512)),
            TestFixtureOptions {
                encrypted: true,
                as_blob: false,
                format: true,
                serve_volume: false,
            },
        )
        .await
    }

    pub async fn new_unencrypted() -> Self {
        Self::open(
            DeviceHolder::new(FakeDevice::new(16384, 512)),
            TestFixtureOptions {
                encrypted: false,
                as_blob: false,
                format: true,
                serve_volume: false,
            },
        )
        .await
    }

    pub async fn open(device: DeviceHolder, options: TestFixtureOptions) -> Self {
        let (filesystem, volume, volumes_directory) = if options.format {
            let filesystem = FxFilesystemBuilder::new().format(true).open(device).await.unwrap();
            let root_volume = root_volume(filesystem.clone()).await.unwrap();
            let store = root_volume
                .new_volume(
                    "vol",
                    if options.encrypted { Some(Arc::new(InsecureCrypt::new())) } else { None },
                )
                .await
                .unwrap();
            let store_object_id = store.store_object_id();
            let volumes_directory =
                VolumesDirectory::new(root_volume, Weak::new(), None).await.unwrap();
            let vol = if options.as_blob {
                FxVolumeAndRoot::new::<BlobDirectory>(Weak::new(), store, store_object_id)
                    .await
                    .unwrap()
            } else {
                FxVolumeAndRoot::new::<FxDirectory>(Weak::new(), store, store_object_id)
                    .await
                    .unwrap()
            };
            (filesystem, vol, volumes_directory)
        } else {
            let filesystem = FxFilesystemBuilder::new().open(device).await.unwrap();
            let root_volume = root_volume(filesystem.clone()).await.unwrap();
            let store = root_volume
                .volume(
                    "vol",
                    if options.encrypted { Some(Arc::new(InsecureCrypt::new())) } else { None },
                )
                .await
                .unwrap();
            let store_object_id = store.store_object_id();
            let volumes_directory =
                VolumesDirectory::new(root_volume, Weak::new(), None).await.unwrap();
            let vol = if options.as_blob {
                FxVolumeAndRoot::new::<BlobDirectory>(Weak::new(), store, store_object_id)
                    .await
                    .unwrap()
            } else {
                FxVolumeAndRoot::new::<FxDirectory>(Weak::new(), store, store_object_id)
                    .await
                    .unwrap()
            };

            (filesystem, vol, volumes_directory)
        };
        let (root, server_end) =
            create_proxy::<fio::DirectoryMarker>().expect("create_proxy failed");
        volume.root().clone().open(
            volume.volume().scope().clone(),
            fio::OpenFlags::DIRECTORY
                | fio::OpenFlags::RIGHT_READABLE
                | fio::OpenFlags::RIGHT_WRITABLE,
            Path::dot(),
            ServerEnd::new(server_end.into_channel()),
        );

        let volume_out_dir = if options.serve_volume {
            let (out_dir, server_end) = fidl::endpoints::create_proxy::<fio::DirectoryMarker>()
                .expect("create_proxy failed");
            volumes_directory
                .serve_volume(&volume, server_end, options.as_blob)
                .expect("serve_volume failed");
            Some(out_dir)
        } else {
            None
        };
        Self {
            state: Some(State { filesystem, volume, volume_out_dir, root, volumes_directory }),
            encrypted: options.encrypted,
        }
    }

    /// Closes the test fixture, shutting down the filesystem. Returns the device, which can be
    /// reused for another TestFixture.
    ///
    /// Ensures that:
    ///   * The filesystem shuts down cleanly.
    ///   * fsck passes.
    ///   * There are no dangling references to the device or the volume.
    pub async fn close(mut self) -> DeviceHolder {
        let State { filesystem, volume, volume_out_dir, root, volumes_directory } =
            std::mem::take(&mut self.state).unwrap();
        if let Some(out_dir) = volume_out_dir {
            out_dir
                .close()
                .await
                .expect("FIDL call failed")
                .map_err(Status::from_raw)
                .expect("close out_dir failed");
        }
        // Close the root node and ensure that there's no remaining references to |vol|, which would
        // indicate a reference cycle or other leak.
        root.close()
            .await
            .expect("FIDL call failed")
            .map_err(Status::from_raw)
            .expect("close root failed");
        volumes_directory.terminate().await;
        std::mem::drop(volumes_directory);
        let store_id = volume.volume().store().store_object_id();

        // Wait for the volume to terminate. If we don't do this, it's possible that we haven't
        // yet noticed that a connection has closed, and so tasks can still be running and they can
        // hold references to the volume which we want to unwrap.
        volume.volume().terminate().await;

        Arc::try_unwrap(volume.into_volume())
            .map_err(|_| "References to volume still exist")
            .unwrap();

        // We have to reopen the filesystem briefly to fsck it. (We could fsck before closing, but
        // there might be pending operations that go through after fsck but before we close the
        // filesystem, and we want to be sure that we catch all possible issues with fsck.)
        filesystem.close().await.expect("close filesystem failed");
        let device = filesystem.take_device().await;
        device.ensure_unique();
        device.reopen(false);
        let filesystem = FxFilesystem::open(device).await.expect("open failed");
        let options = FsckOptions {
            fail_on_warning: true,
            on_error: Box::new(|err: &FsckIssue| {
                eprintln!("Fsck error: {:?}", err);
            }),
            ..Default::default()
        };
        fsck_with_options(filesystem.clone(), &options).await.expect("fsck failed");
        fsck_volume_with_options(
            filesystem.as_ref(),
            &options,
            store_id,
            if self.encrypted { Some(Arc::new(InsecureCrypt::new())) } else { None },
        )
        .await
        .expect("fsck_volume failed");

        filesystem.close().await.expect("close filesystem failed");
        let device = filesystem.take_device().await;
        device.ensure_unique();
        device.reopen(false);

        device
    }

    pub fn root(&self) -> &fio::DirectoryProxy {
        &self.state.as_ref().unwrap().root
    }

    pub fn fs(&self) -> &Arc<FxFilesystem> {
        &self.state.as_ref().unwrap().filesystem
    }

    pub fn volume(&self) -> &FxVolumeAndRoot {
        &self.state.as_ref().unwrap().volume
    }

    pub fn volumes_directory(&self) -> &Arc<VolumesDirectory> {
        &self.state.as_ref().unwrap().volumes_directory
    }

    pub fn volume_out_dir(&self) -> &fio::DirectoryProxy {
        self.state
            .as_ref()
            .unwrap()
            .volume_out_dir
            .as_ref()
            .expect("Did you forget to set `serve_volume` in TestFixtureOptions?")
    }
}

impl Drop for TestFixture {
    fn drop(&mut self) {
        assert!(self.state.is_none(), "Did you forget to call TestFixture::close?");
    }
}

pub async fn close_file_checked(file: fio::FileProxy) {
    file.sync().await.expect("FIDL call failed").map_err(Status::from_raw).expect("sync failed");
    file.close().await.expect("FIDL call failed").map_err(Status::from_raw).expect("close failed");
}

pub async fn close_dir_checked(dir: fio::DirectoryProxy) {
    dir.close().await.expect("FIDL call failed").map_err(Status::from_raw).expect("close failed");
}

// Utility function to open a new node connection under |dir|.
pub async fn open_file(
    dir: &fio::DirectoryProxy,
    flags: fio::OpenFlags,
    path: &str,
) -> Result<fio::FileProxy, Error> {
    let (proxy, server_end) = create_proxy::<fio::FileMarker>().expect("create_proxy failed");
    dir.open(flags, fio::ModeType::empty(), path, ServerEnd::new(server_end.into_channel()))?;
    let _: Vec<_> = proxy.query().await?;
    Ok(proxy)
}

// Like |open_file|, but asserts if the open call fails.
pub async fn open_file_checked(
    dir: &fio::DirectoryProxy,
    flags: fio::OpenFlags,
    path: &str,
) -> fio::FileProxy {
    open_file(dir, flags, path).await.expect("open_file failed")
}

// Utility function to open a new node connection under |dir|.
pub async fn open_dir(
    dir: &fio::DirectoryProxy,
    flags: fio::OpenFlags,
    path: &str,
) -> Result<fio::DirectoryProxy, Error> {
    let (proxy, server_end) = create_proxy::<fio::DirectoryMarker>().expect("create_proxy failed");
    dir.open(flags, fio::ModeType::empty(), path, ServerEnd::new(server_end.into_channel()))?;
    let _: Vec<_> = proxy.query().await?;
    Ok(proxy)
}

// Like |open_dir|, but asserts if the open call fails.
pub async fn open_dir_checked(
    dir: &fio::DirectoryProxy,
    flags: fio::OpenFlags,
    path: &str,
) -> fio::DirectoryProxy {
    open_dir(dir, flags, path).await.expect("open_dir failed")
}

/// Utility function to write to an `FxFile`.
pub async fn write_at(file: &FxFile, offset: u64, content: &[u8]) -> Result<usize, Error> {
    let stream = zx::Stream::create(zx::StreamOptions::MODE_WRITE, file.vmo(), 0)
        .context("stream create failed")?;
    let content = content.to_vec();
    unblock(move || {
        stream
            .writev_at(zx::StreamWriteOptions::empty(), offset, &[&content])
            .context("stream write failed")
    })
    .await
}
