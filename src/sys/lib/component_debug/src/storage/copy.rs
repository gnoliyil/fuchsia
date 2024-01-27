// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::io::{Directory, LocalDirectory, RemoteDirectory},
    crate::path::{add_source_filename_to_path_if_absent, HostOrRemotePath, REMOTE_PATH_HELP},
    anyhow::{anyhow, bail, Result},
    fidl::endpoints::create_proxy,
    fidl_fuchsia_io as fio,
    fidl_fuchsia_sys2::StorageAdminProxy,
    std::path::PathBuf,
};

/// Transfer a file between the host machine and the Fuchsia device.
/// Can be used to upload a file to or from the Fuchsia device.
///
/// # Arguments
/// * `storage_admin`: The StorageAdminProxy.
/// * `source_path`: The path to a file on the host machine to be uploaded to the device or to a file on the device to be downloaded on the host machine
/// * `destination_path`: The path and filename on the target component or the host machine where to save the file
pub async fn copy(
    storage_admin: StorageAdminProxy,
    source_path: String,
    destination_path: String,
) -> Result<()> {
    let (dir_proxy, server) = create_proxy::<fio::DirectoryMarker>()?;
    let server = server.into_channel();
    let storage_dir = RemoteDirectory::from_proxy(dir_proxy);

    match (HostOrRemotePath::parse(&source_path), HostOrRemotePath::parse(&destination_path)) {
        (HostOrRemotePath::Remote(source), HostOrRemotePath::Host(destination_path)) => {
            // Copying from remote to host
            storage_admin
                .open_component_storage_by_id(&source.remote_id, server.into())
                .await?
                .map_err(|e| anyhow!("Could not open component storage: {:?}", e))?;

            let destination_dir = LocalDirectory::new();
            do_copy(&storage_dir, &source.relative_path, &destination_dir, &destination_path).await
        }
        (HostOrRemotePath::Host(source_path), HostOrRemotePath::Remote(destination)) => {
            // Copying from host to remote
            storage_admin
                .open_component_storage_by_id(&destination.remote_id, server.into())
                .await?
                .map_err(|e| anyhow!("Could not open component storage: {:?}", e))?;

            let source_dir = LocalDirectory::new();
            do_copy(&source_dir, &source_path, &storage_dir, &destination.relative_path).await
        }
        _ => {
            bail!("One path must be remote and the other must be host. {}", REMOTE_PATH_HELP)
        }
    }
}

async fn do_copy<S: Directory, D: Directory>(
    source_dir: &S,
    source_path: &PathBuf,
    destination_dir: &D,
    destination_path: &PathBuf,
) -> Result<()> {
    let destination_path_path =
        add_source_filename_to_path_if_absent(destination_dir, source_path, destination_path)
            .await?;

    let data = source_dir.read_file_bytes(source_path).await?;
    destination_dir.write_file(destination_path_path, &data).await
}

////////////////////////////////////////////////////////////////////////////////
// tests

#[cfg(test)]
mod test {
    use {
        super::*,
        crate::storage::test::{
            node_to_file, setup_fake_storage_admin, setup_fake_storage_admin_with_tmp,
        },
        fidl_fuchsia_io as fio,
        futures::TryStreamExt,
        std::collections::HashMap,
        std::fs::{read, write},
        tempfile::tempdir,
    };

    const EXPECTED_DATA: [u8; 4] = [0x0, 0x1, 0x2, 0x3];

    // TODO(xbhatnag): Replace this mock with something more robust like VFS.
    // Currently VFS is not cross-platform.
    fn setup_fake_directory(mut root_dir: fio::DirectoryRequestStream) {
        fuchsia_async::Task::local(async move {
            // Serve the root directory
            // Rewind on root directory should succeed
            let request = root_dir.try_next().await;
            if let Ok(Some(fio::DirectoryRequest::Open { path, flags, object, .. })) = request {
                if path == "from_host" {
                    assert!(flags.intersects(fio::OpenFlags::CREATE));
                    setup_fake_file_from_host(node_to_file(object));
                } else if path == "from_device" {
                    setup_fake_file_from_device(node_to_file(object));
                } else {
                    panic!("incorrect path: {}", path);
                }
            } else {
                panic!("did not get open request: {:?}", request)
            }
        })
        .detach();
    }

    fn setup_fake_file_from_host(mut file: fio::FileRequestStream) {
        fuchsia_async::Task::local(async move {
            // Serve the root directory
            // Truncating the file should succeed
            let request = file.try_next().await;
            if let Ok(Some(fio::FileRequest::Resize { length, responder })) = request {
                assert_eq!(length, 0);
                responder.send(Ok(())).unwrap();
            } else {
                panic!("did not get resize request: {:?}", request)
            }

            // Writing the file should succeed
            let request = file.try_next().await;
            if let Ok(Some(fio::FileRequest::Write { data, responder })) = request {
                assert_eq!(data, EXPECTED_DATA);
                responder.send(Ok(data.len() as u64)).unwrap();
            } else {
                panic!("did not get write request: {:?}", request)
            }

            // Closing file should succeed
            let request = file.try_next().await;
            if let Ok(Some(fio::FileRequest::Close { responder })) = request {
                responder.send(Ok(())).unwrap();
            } else {
                panic!("did not get close request: {:?}", request)
            }
        })
        .detach();
    }

    fn setup_fake_file_from_device(mut file: fio::FileRequestStream) {
        fuchsia_async::Task::local(async move {
            // Serve the root directory
            // Reading the file should succeed
            let request = file.try_next().await;
            if let Ok(Some(fio::FileRequest::Read { responder, .. })) = request {
                responder.send(Ok(&EXPECTED_DATA)).unwrap();
            } else {
                panic!("did not get read request: {:?}", request)
            }

            // Reading the file should not return any more data
            let request = file.try_next().await;
            if let Ok(Some(fio::FileRequest::Read { responder, .. })) = request {
                responder.send(Ok(&[])).unwrap();
            } else {
                panic!("did not get read request: {:?}", request)
            }

            // Closing file should succeed
            let request = file.try_next().await;
            if let Ok(Some(fio::FileRequest::Close { responder })) = request {
                responder.send(Ok(())).unwrap();
            } else {
                panic!("did not get close request: {:?}", request)
            }
        })
        .detach();
    }

    #[fuchsia::test]
    async fn test_copy_host_to_device() -> Result<()> {
        let dir = tempdir().unwrap();
        let storage_admin = setup_fake_storage_admin_with_tmp("123456", HashMap::new());
        let from_host_filepath = dir.path().join("from_host");
        write(&from_host_filepath, &EXPECTED_DATA).unwrap();
        copy(
            storage_admin,
            from_host_filepath.display().to_string(),
            "123456::from_host".to_string(),
        )
        .await
    }

    #[fuchsia::test]
    async fn test_copy_host_to_device_different_file_names() -> Result<()> {
        let dir = tempdir().unwrap();
        let storage_admin = setup_fake_storage_admin_with_tmp("123456", HashMap::new());
        let from_host_filepath = dir.path().join("from_host");
        write(&from_host_filepath, &EXPECTED_DATA).unwrap();
        copy(
            storage_admin,
            from_host_filepath.display().to_string(),
            "123456::from_host_test".to_string(),
        )
        .await
    }

    #[fuchsia::test]
    async fn test_copy_host_to_device_infer_path() -> Result<()> {
        let dir = tempdir().unwrap();
        let storage_admin = setup_fake_storage_admin_with_tmp("123456", HashMap::new());
        let from_host_filepath = dir.path().join("from_host");
        write(&from_host_filepath, &EXPECTED_DATA).unwrap();
        copy(storage_admin, from_host_filepath.display().to_string(), "123456::".to_string()).await
    }

    #[fuchsia::test]
    async fn test_copy_host_to_device_infer_slash_path() -> Result<()> {
        let dir = tempdir().unwrap();
        let storage_admin = setup_fake_storage_admin_with_tmp("123456", HashMap::new());
        let from_host_filepath = dir.path().join("from_host");
        write(&from_host_filepath, &EXPECTED_DATA).unwrap();
        copy(storage_admin, from_host_filepath.display().to_string(), "123456::/".to_string()).await
    }

    #[fuchsia::test]
    async fn test_copy_host_to_device_overwrite_file() -> Result<()> {
        let dir = tempdir().unwrap();
        let mut seed_files = HashMap::new();
        seed_files.insert("from_host", "Lorem Ipsum");
        let storage_admin = setup_fake_storage_admin_with_tmp("123456", seed_files);
        let from_host_filepath = dir.path().join("from_host");
        write(&from_host_filepath, &EXPECTED_DATA).unwrap();
        copy(
            storage_admin,
            from_host_filepath.display().to_string(),
            "123456::from_host".to_string(),
        )
        .await
    }

    #[fuchsia::test]
    async fn test_copy_host_to_device_populated_directory() -> Result<()> {
        let dir = tempdir().unwrap();
        let mut seed_files = HashMap::new();

        seed_files.insert("foo.txt", "Lorem Ipsum");

        let storage_admin = setup_fake_storage_admin_with_tmp("123456", seed_files);
        let from_host_filepath = dir.path().join("from_host");
        write(&from_host_filepath, &EXPECTED_DATA).unwrap();
        copy(
            storage_admin,
            from_host_filepath.display().to_string(),
            "123456::from_host".to_string(),
        )
        .await
    }

    #[fuchsia::test]
    async fn test_copy_device_to_host_infer_path() -> Result<()> {
        let dir = tempdir().unwrap();
        let storage_admin = setup_fake_storage_admin("123456", setup_fake_directory);
        let dest_filepath = dir.path();

        copy(storage_admin, "123456::from_device".to_string(), dest_filepath.display().to_string())
            .await?;

        let final_path = dest_filepath.join("from_device");
        let actual_data = read(final_path).unwrap();
        assert_eq!(actual_data, EXPECTED_DATA);
        Ok(())
    }

    #[fuchsia::test]
    async fn test_copy_device_to_host_infer_slash_path() -> Result<()> {
        let dir = tempdir().unwrap();
        let storage_admin = setup_fake_storage_admin("123456", setup_fake_directory);
        let dest_filepath = dir.path();

        copy(
            storage_admin,
            "123456::from_device".to_string(),
            dest_filepath.display().to_string() + "/",
        )
        .await?;

        let final_path = dest_filepath.join("from_device");
        let actual_data = read(final_path).unwrap();
        assert_eq!(actual_data, EXPECTED_DATA);
        Ok(())
    }

    #[fuchsia::test]
    async fn test_copy_device_to_host() -> Result<()> {
        let dir = tempdir().unwrap();
        let storage_admin = setup_fake_storage_admin("123456", setup_fake_directory);
        let dest_filepath = dir.path().join("from_device");
        copy(storage_admin, "123456::from_device".to_string(), dest_filepath.display().to_string())
            .await?;
        let actual_data = read(dest_filepath).unwrap();
        assert_eq!(actual_data, EXPECTED_DATA);
        Ok(())
    }
}
