// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![cfg(test)]

use {
    anyhow::Error,
    assert_matches::assert_matches,
    fdio::{SpawnAction, SpawnOptions},
    fidl_fuchsia_io as fio,
    fidl_fuchsia_mem::Buffer,
    fidl_fuchsia_storage_ext4::{MountVmoResult, Server_Marker, ServiceMarker, Success},
    fuchsia_fs,
    fuchsia_runtime::{HandleInfo, HandleType},
    fuchsia_zircon::{self as zx, AsHandleRef, DurationNum},
    maplit::hashmap,
    ramdevice_client::RamdiskClient,
    remote_block_device::{BlockClient, RemoteBlockClient},
    sha2::{Digest, Sha256},
    std::collections::HashMap,
    std::io::{self, Read, Seek},
    std::{ffi::CString, fs},
    test_case::test_case,
};

const RAMDISK_BLOCK_SIZE: u64 = 1024;
const RAMDISK_BLOCK_COUNT: u64 = 16 * 1024;

async fn make_ramdisk() -> (RamdiskClient, RemoteBlockClient) {
    let client = RamdiskClient::builder(RAMDISK_BLOCK_SIZE, RAMDISK_BLOCK_COUNT);
    let ramdisk = client.build().await.expect("RamdiskClientBuilder.build() failed");
    let client_end = ramdisk.open().await.expect("ramdisk.open failed");
    let proxy = client_end.into_proxy().expect("into_proxy failed");
    let remote_block_device = RemoteBlockClient::new(proxy).await.expect("new failed");

    assert_eq!(remote_block_device.block_size(), 1024);
    (ramdisk, remote_block_device)
}

#[test_case(
    "/pkg/data/extents.img",
    hashmap!{
        "largefile".to_string() => "de2cf635ae4e0e727f1e412f978001d6a70d2386dc798d4327ec8c77a8e4895d".to_string(),
        "smallfile".to_string() => "5891b5b522d5df086d0ff0b110fbd9d21bb4fc7163af34d08286a2e846f6be03".to_string(),
        "sparsefile".to_string() => "3f411e42c1417cd8845d7144679812be3e120318d843c8c6e66d8b2c47a700e9".to_string(),
        "a/multi/dir/path/within/this/crowded/extents/test/img/empty".to_string() => "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855".to_string(),
    };
    "fs with multiple files with multiple extents")]
#[test_case(
    "/pkg/data/1file.img",
    hashmap!{
        "file1".to_string() => "6bc35bfb2ca96c75a1fecde205693c19a827d4b04e90ace330048f3e031487dd".to_string(),
    };
    "fs with one small file")]
#[test_case(
    "/pkg/data/nest.img",
    hashmap!{
        "file1".to_string() => "6bc35bfb2ca96c75a1fecde205693c19a827d4b04e90ace330048f3e031487dd".to_string(),
        "inner/file2".to_string() => "215ca145cbac95c9e2a6f5ff91ca1887c837b18e5f58fd2a7a16e2e5a3901e10".to_string(),
    };
    "fs with a single directory")]
#[fuchsia::test]
async fn ext4_server_mounts_block_device(
    ext4_path: &str,
    file_hashes: HashMap<String, String>,
) -> Result<(), Error> {
    let mut file_buf = io::BufReader::new(fs::File::open(ext4_path)?);
    let size = file_buf.seek(io::SeekFrom::End(0))?;
    file_buf.seek(io::SeekFrom::Start(0))?;

    let mut temp_buf = vec![0u8; size as usize];
    {
        file_buf.read_exact(&mut temp_buf)?;
    }

    let (ramdisk, remote_block_device) = make_ramdisk().await;
    remote_block_device.write_at(temp_buf[..].into(), 0).await.expect("write_at failed");
    // Close the connection to the block device so the ext4 server can connect.
    remote_block_device.close().await.unwrap();

    let ext4_binary_path = CString::new("/pkg/bin/ext4_readonly").unwrap();
    let (dir_proxy, dir_server) = fidl::endpoints::create_proxy::<fio::DirectoryMarker>()?;

    let _process = scoped_task::spawn_etc(
        &scoped_task::job_default(),
        SpawnOptions::CLONE_ALL,
        &ext4_binary_path,
        &[&ext4_binary_path],
        None,
        &mut [
            SpawnAction::add_handle(HandleType::DirectoryRequest.into(), dir_server.into()),
            SpawnAction::add_handle(
                HandleInfo::new(HandleType::User0, 1),
                ramdisk.open().await.expect("ramdisk.open").into(),
            ),
        ],
    )
    .unwrap();

    for (file_path, expected_hash) in &file_hashes {
        let file = fuchsia_fs::directory::open_file_no_describe(
            &dir_proxy,
            file_path,
            fuchsia_fs::OpenFlags::RIGHT_READABLE,
        )?;
        let mut hasher = Sha256::new();
        hasher.update(&fuchsia_fs::file::read(&file).await?);
        assert_eq!(*expected_hash, hex::encode(hasher.finalize()));
    }

    Ok(())
}

#[fuchsia::test]
async fn ext4_server_mounts_block_device_and_dies_on_close() -> Result<(), Error> {
    let mut file_buf = io::BufReader::new(fs::File::open("/pkg/data/nest.img")?);
    let size = file_buf.seek(io::SeekFrom::End(0))?;
    file_buf.seek(io::SeekFrom::Start(0))?;

    let mut temp_buf = vec![0u8; size as usize];
    file_buf.read_exact(&mut temp_buf)?;

    let (ramdisk, remote_block_device) = make_ramdisk().await;
    remote_block_device.write_at(temp_buf[..].into(), 0).await.expect("write_at failed");
    // Close the connection to the block device so the ext4 server can connect.
    remote_block_device.close().await.unwrap();

    let ext4_binary_path = CString::new("/pkg/bin/ext4_readonly").unwrap();
    let (dir_proxy, dir_server) = fidl::endpoints::create_proxy::<fio::DirectoryMarker>()?;

    let process = scoped_task::spawn_etc(
        &scoped_task::job_default(),
        SpawnOptions::CLONE_ALL,
        &ext4_binary_path,
        &[&ext4_binary_path],
        None,
        &mut [
            SpawnAction::add_handle(HandleType::DirectoryRequest.into(), dir_server.into()),
            SpawnAction::add_handle(
                HandleInfo::new(HandleType::User0, 1),
                ramdisk.open().await.expect("ramdisk.open").into(),
            ),
        ],
    )
    .unwrap();

    std::mem::drop(dir_proxy);
    process.wait_handle(zx::Signals::TASK_TERMINATED, zx::Time::after(5.seconds()))?;
    Ok(())
}

#[fuchsia::test]
async fn ext4_server_mounts_vmo_one_file() -> Result<(), Error> {
    let ext4 = fuchsia_component::client::connect_to_protocol::<Server_Marker>()
        .expect("Failed to connect to service");

    let mut file_buf = io::BufReader::new(fs::File::open("/pkg/data/1file.img")?);
    let size = file_buf.seek(io::SeekFrom::End(0))?;
    file_buf.seek(io::SeekFrom::Start(0))?;

    let mut temp_buf = vec![0u8; size as usize];
    file_buf.read_exact(&mut temp_buf)?;

    let vmo = zx::Vmo::create(size)?;
    vmo.write(&temp_buf, 0)?;
    let mut buf = Buffer { vmo, size };

    let (dir_proxy, dir_server) = fidl::endpoints::create_proxy::<fio::DirectoryMarker>()?;
    let result = ext4.mount_vmo(&mut buf, fio::OpenFlags::RIGHT_READABLE, dir_server).await;
    assert_matches!(result, Ok(MountVmoResult::Success(Success {})));

    let file = fuchsia_fs::directory::open_file_no_describe(
        &dir_proxy,
        "file1",
        fuchsia_fs::OpenFlags::RIGHT_READABLE,
    )?;
    assert_eq!("file1 contents.\n".to_string(), fuchsia_fs::file::read_to_string(&file).await?);
    Ok(())
}

#[fuchsia::test]
async fn ext4_server_mounts_vmo_nested_dirs() -> Result<(), Error> {
    let ext4 = fuchsia_component::client::connect_to_protocol::<Server_Marker>()
        .expect("Failed to connect to service");

    let mut file_buf = io::BufReader::new(fs::File::open("/pkg/data/nest.img")?);
    let size = file_buf.seek(io::SeekFrom::End(0))?;
    file_buf.seek(io::SeekFrom::Start(0))?;

    let mut temp_buf = vec![0u8; size as usize];
    file_buf.read_exact(&mut temp_buf)?;

    let vmo = zx::Vmo::create(size)?;
    vmo.write(&temp_buf, 0)?;
    let mut buf = Buffer { vmo, size };

    let (dir_proxy, dir_server) = fidl::endpoints::create_proxy::<fio::DirectoryMarker>()?;
    let result = ext4.mount_vmo(&mut buf, fio::OpenFlags::RIGHT_READABLE, dir_server).await;
    assert_matches!(result, Ok(MountVmoResult::Success(Success {})));

    let file1 = fuchsia_fs::directory::open_file_no_describe(
        &dir_proxy,
        "file1",
        fuchsia_fs::OpenFlags::RIGHT_READABLE,
    )?;
    assert_eq!("file1 contents.\n".to_string(), fuchsia_fs::file::read_to_string(&file1).await?);

    let file2 = fuchsia_fs::directory::open_file_no_describe(
        &dir_proxy,
        "inner/file2",
        fuchsia_fs::OpenFlags::RIGHT_READABLE,
    )?;
    assert_eq!("file2 contents.\n".to_string(), fuchsia_fs::file::read_to_string(&file2).await?);
    Ok(())
}

#[fuchsia::test]
async fn ext4_unified_service_mounts_vmo() -> Result<(), Error> {
    let ext4_service = fuchsia_component::client::connect_to_service::<ServiceMarker>()
        .expect("Failed to connect to service");
    let ext4 = ext4_service.server()?;

    let mut file_buf = io::BufReader::new(fs::File::open("/pkg/data/nest.img")?);
    let size = file_buf.seek(io::SeekFrom::End(0))?;
    file_buf.seek(io::SeekFrom::Start(0))?;

    let mut temp_buf = vec![0u8; size as usize];
    file_buf.read_exact(&mut temp_buf)?;

    let vmo = zx::Vmo::create(size)?;
    vmo.write(&temp_buf, 0)?;
    let mut buf = Buffer { vmo, size };

    let (dir_proxy, dir_server) = fidl::endpoints::create_proxy::<fio::DirectoryMarker>()?;
    let result = ext4.mount_vmo(&mut buf, fio::OpenFlags::RIGHT_READABLE, dir_server).await;
    assert_matches!(result, Ok(MountVmoResult::Success(Success {})));

    let file1 = fuchsia_fs::directory::open_file_no_describe(
        &dir_proxy,
        "file1",
        fuchsia_fs::OpenFlags::RIGHT_READABLE,
    )?;
    assert_eq!("file1 contents.\n".to_string(), fuchsia_fs::file::read_to_string(&file1).await?);

    let file2 = fuchsia_fs::directory::open_file_no_describe(
        &dir_proxy,
        "inner/file2",
        fuchsia_fs::OpenFlags::RIGHT_READABLE,
    )?;
    assert_eq!("file2 contents.\n".to_string(), fuchsia_fs::file::read_to_string(&file2).await?);
    Ok(())
}
