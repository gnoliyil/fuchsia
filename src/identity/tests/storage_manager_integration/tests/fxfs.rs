// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::Error,
    assert_matches::assert_matches,
    fidl_fuchsia_io as fio,
    fs_management::{
        filesystem::{Filesystem, ServingMultiVolumeFilesystem},
        FSConfig, Fxfs as FxfsConfig,
    },
    fuchsia_zircon::Status,
    ramdevice_client::{RamdiskClient, RamdiskClientBuilder},
    rand::{thread_rng, Rng},
    storage_manager::{
        fxfs::{Args as FxfsArgs, Fxfs},
        StorageManager,
    },
};

const RAMCTL_PATH: &str = "/dev/sys/platform/00:00:2d/ramctl";
const BLOCK_SIZE: u64 = 4096;
const BLOCK_COUNT: u64 = 1024; // 4MB RAM ought to be good enough
const ACCOUNT_LABEL: &str = "account";

fn ramdisk() -> RamdiskClient {
    ramdevice_client::wait_for_device(RAMCTL_PATH, std::time::Duration::from_secs(60))
        .expect("Could not wait for ramctl from isolated-devmgr");

    RamdiskClientBuilder::new(BLOCK_SIZE, BLOCK_COUNT).build().expect("Could not create ramdisk")
}

fn new_fs<FSC: FSConfig>(ramdisk: &RamdiskClient, config: FSC) -> Filesystem<FSC> {
    Filesystem::from_channel(ramdisk.open().unwrap().into_channel(), config).unwrap()
}

async fn make_ramdisk_and_filesystem(
) -> Result<(RamdiskClient, Filesystem<FxfsConfig>, ServingMultiVolumeFilesystem), Error> {
    let ramdisk = ramdisk();

    let mut fxfs: Filesystem<_> = new_fs(&ramdisk, FxfsConfig::default());

    fxfs.format().await.expect("failed to format fxfs");

    let fs: ServingMultiVolumeFilesystem =
        fxfs.serve_multi_volume().await.expect("failed to serve fxfs");

    Ok::<(RamdiskClient, Filesystem<FxfsConfig>, ServingMultiVolumeFilesystem), Error>((
        ramdisk, fxfs, fs,
    ))
}

fn new_key() -> [u8; 32] {
    let mut bits = [0_u8; 32];
    thread_rng().fill(&mut bits[..]);
    bits
}

fn new_storage_manager(fs: &mut ServingMultiVolumeFilesystem) -> Fxfs {
    Fxfs::new(
        FxfsArgs::builder()
            .volume_label(ACCOUNT_LABEL.to_string())
            .filesystem_dir(
                fuchsia_fs::clone_directory(fs.exposed_dir(), fio::OpenFlags::CLONE_SAME_RIGHTS)
                    .unwrap(),
            )
            .use_unique_crypt_name_for_test(true)
            .build(),
    )
}

#[fuchsia::test]
async fn test_lifecycle() -> Result<(), Error> {
    let (_ramdisk, _filesystem, mut fs): (_, _, _) = make_ramdisk_and_filesystem().await?;
    let fxfs_storage_manager = new_storage_manager(&mut fs);

    let key = new_key();
    let expected_contents = b"some data";

    // "/volumes/account" doesn't exist yet.
    assert!(!fs.has_volume(ACCOUNT_LABEL).await.expect("has_volume failed"));

    // Provisioning the volume creates and mounts it. The volume is now unlocked.
    assert_matches!(fxfs_storage_manager.provision(&key).await, Ok(()));

    // "/volumes/account" has been created by the call to .provision().
    assert!(fs.has_volume(ACCOUNT_LABEL).await.expect("has_volume failed"));

    {
        let root_dir = fxfs_storage_manager.get_root_dir().await.unwrap();

        // We can write a file to it,
        {
            let file = fuchsia_fs::directory::open_file(
                &root_dir,
                "test",
                fio::OpenFlags::CREATE
                    | fio::OpenFlags::RIGHT_READABLE
                    | fio::OpenFlags::RIGHT_WRITABLE,
            )
            .await
            .expect("create file");

            let bytes_written = file
                .write(expected_contents)
                .await
                .expect("file write")
                .map_err(Status::from_raw)
                .expect("failed to write content");
            assert_eq!(bytes_written, expected_contents.len() as u64);
        }

        // Drop |root_dir| when it falls out-of-scope here, which allows us to
        // close channels to the directory and eventually unmount the volume.

        // TODO(jbuckland): Once it is possible to unmount the volume without
        // closing channels, write tests which (a) lock before closing, and (b)
        // try and fail to read |file|.
    }

    // And then lock the volume.
    assert_matches!(fxfs_storage_manager.lock_storage().await, Ok(()));

    // We should be able to unlock it with the same key,
    assert_matches!(fxfs_storage_manager.unlock_storage(&key).await, Ok(()));

    // And view that same file.
    {
        let root_dir = fxfs_storage_manager.get_root_dir().await.unwrap();
        let file =
            fuchsia_fs::directory::open_file(&root_dir, "test", fio::OpenFlags::RIGHT_READABLE)
                .await
                .expect("create file");

        let actual_contents = fuchsia_fs::file::read(&file).await.expect("read file");
        assert_eq!(&actual_contents, expected_contents);
    }

    // Finally, destroy the storage manager,...
    assert_matches!(fxfs_storage_manager.destroy().await, Ok(()));

    // which means that we cannot access the root directory.
    assert_matches!(fxfs_storage_manager.get_root_dir().await, Err(_));

    let () = fs.shutdown().await.unwrap();

    Ok(())
}

#[fuchsia::test]
async fn test_get_root_dir_before_provision() -> Result<(), Error> {
    let (_ramdisk, _filesystem, mut fs): (_, _, _) = make_ramdisk_and_filesystem().await?;
    let fxfs_storage_manager = new_storage_manager(&mut fs);

    // Calling .get_root_dir() before .provision() fails, since the root
    // directory has not yet been set up.
    assert_matches!(fxfs_storage_manager.get_root_dir().await, Err(_));

    let () = fs.shutdown().await.unwrap();
    Ok(())
}

#[fuchsia::test]
async fn test_get_root_dir_while_locked() -> Result<(), Error> {
    let (_ramdisk, _filesystem, mut fs): (_, _, _) = make_ramdisk_and_filesystem().await?;
    let fxfs_storage_manager = new_storage_manager(&mut fs);
    let key = new_key();

    // Calling .get_root_dir() while locked fails, since the root
    // directory is inaccessible.
    assert_matches!(fxfs_storage_manager.provision(&key).await, Ok(()));
    assert_matches!(fxfs_storage_manager.lock_storage().await, Ok(()));
    assert_matches!(fxfs_storage_manager.get_root_dir().await, Err(_));

    let () = fs.shutdown().await.unwrap();
    Ok(())
}
