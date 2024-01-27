// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![warn(clippy::all)]
#![allow(clippy::let_unit_value)]

use {
    anyhow::{anyhow, Context as _, Error},
    fidl::endpoints::ServerEnd,
    fidl_fuchsia_io as fio,
    fidl_fuchsia_pkg::{GetBlobError, GetMetadataError},
    fidl_fuchsia_pkg_ext::{BlobId, RepositoryUrl},
    fuchsia_syslog::fx_log_info,
};

pub struct LocalMirrorManager {
    blobs_dir: fio::DirectoryProxy,
    metadata_dir: fio::DirectoryProxy,
}

impl LocalMirrorManager {
    pub async fn new(usb_dir: &fio::DirectoryProxy) -> Result<Self, Error> {
        let blobs_dir =
            fuchsia_fs::directory::open_directory(usb_dir, "blobs", fio::OpenFlags::RIGHT_READABLE)
                .await
                .context("while opening blobs dir")?;

        let metadata_dir = fuchsia_fs::directory::open_directory(
            usb_dir,
            "repository_metadata",
            fio::OpenFlags::RIGHT_READABLE,
        )
        .await
        .context("while opening metadata dir")?;

        Ok(Self { blobs_dir, metadata_dir })
    }

    /// Connects the file in the USB metadata directory to the passed-in handle.
    /// Note we don't need to validate the path because the open calls are forwarded
    /// to a directory that only contains metadata, and Fuchsia filesystems prevent
    /// accessing data that is outside the directory.
    /// https://fuchsia.dev/fuchsia-src/concepts/filesystems/dotdot?hl=en
    pub async fn get_metadata(
        &self,
        repo_url: RepositoryUrl,
        path: &str,
        metadata: ServerEnd<fio::FileMarker>,
    ) -> Result<(), GetMetadataError> {
        let path = format!("{}/{}", repo_url.url().host(), path);
        let () = self
            .metadata_dir
            .open(
                fio::OpenFlags::RIGHT_READABLE
                    | fio::OpenFlags::NOT_DIRECTORY
                    | fio::OpenFlags::DESCRIBE,
                fio::ModeType::empty(),
                &path,
                ServerEnd::new(metadata.into_channel()),
            )
            .map_err(|e| {
                fx_log_info!("while opening metadata {}: {:#}", path, anyhow!(e));
                GetMetadataError::ErrorOpeningMetadata
            })?;

        Ok(())
    }

    /// Connects the file in the USB blobs directory to the passed-in handle.
    pub async fn get_blob(
        &self,
        blob_id: BlobId,
        blob: ServerEnd<fio::FileMarker>,
    ) -> Result<(), GetBlobError> {
        let blob_id = blob_id.to_string();
        let (first, last) = blob_id.split_at(2);
        let path = format!("{first}/{last}");

        let () = self
            .blobs_dir
            .open(
                fio::OpenFlags::RIGHT_READABLE
                    | fio::OpenFlags::NOT_DIRECTORY
                    | fio::OpenFlags::DESCRIBE,
                fio::ModeType::empty(),
                &path,
                ServerEnd::new(blob.into_channel()),
            )
            .map_err(|e| {
                fx_log_info!("while opening blob {}: {:#}", path, anyhow!(e));
                GetBlobError::ErrorOpeningBlob
            })?;

        Ok(())
    }
}
