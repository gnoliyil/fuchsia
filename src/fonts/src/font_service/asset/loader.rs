// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    super::collection::AssetCollectionError,
    anyhow::Error,
    async_trait::async_trait,
    fidl::endpoints::create_proxy,
    fidl_fuchsia_io as io, fidl_fuchsia_mem as mem,
    fidl_fuchsia_pkg::FontResolverMarker,
    fuchsia_component::client::connect_to_protocol,
    fuchsia_trace as trace, fuchsia_zircon as zx,
    manifest::v2,
    std::{fs::File, path::Path},
};

/// A trait that covers the interactions of the font service with `fuchsia.pkg.FontResolver` and
/// font asset VMOs. Intended for easier testing.
#[async_trait]
pub trait AssetLoader {
    /// Resolves a font package to its `Directory`.
    async fn fetch_package_directory(
        &self,
        package_locator: &v2::PackageLocator,
    ) -> Result<io::DirectoryProxy, AssetCollectionError>;

    /// Gets a `VMO` handle to the [`Asset`] at a local `path`.
    fn load_vmo_from_path(&self, path: &Path) -> Result<mem::Buffer, AssetCollectionError>;

    /// Gets a VMO handle to the file with the given `file_name` within the given directory.
    /// The `package_locator` is used for detailed error messages.
    async fn load_vmo_from_directory_proxy(
        &self,
        directory_proxy: io::DirectoryProxy,
        package_locator: &v2::PackageLocator,
        file_name: &str,
    ) -> Result<mem::Buffer, AssetCollectionError>;
}

/// Real implementation of [`AssetLoader`].
pub struct AssetLoaderImpl {}

impl AssetLoaderImpl {
    /// Creates a new instance of `AssetLoaderImpl`.
    pub fn new() -> Self {
        AssetLoaderImpl {}
    }
}

/// This implementation is currently covered by integration tests only.
/// TODO(fxbug.dev/48649): Unit tests.
#[async_trait]
impl AssetLoader for AssetLoaderImpl {
    async fn fetch_package_directory(
        &self,
        package_locator: &v2::PackageLocator,
    ) -> Result<io::DirectoryProxy, AssetCollectionError> {
        let package_url = package_locator.url.to_string();
        trace::duration!(
            "fonts",
            "asset:fetcher:fetch_package_directory",
            "package_url" => &package_url[..]);

        // Get directory handle from FontResolver
        let font_resolver = connect_to_protocol::<FontResolverMarker>()
            .map_err(|e| AssetCollectionError::ServiceConnectionError(e.into()))?;
        let (dir_proxy, dir_request) = create_proxy::<io::DirectoryMarker>()
            .map_err(|e| AssetCollectionError::ServiceConnectionError(e.into()))?;

        let response = font_resolver.resolve(&package_url, dir_request).await.map_err(|e| {
            AssetCollectionError::PackageResolverError(package_locator.clone(), e.into())
        })?;
        let () = response.map_err(|i| {
            AssetCollectionError::PackageResolverError(
                package_locator.clone(),
                zx::Status::from_raw(i).into(),
            )
        })?;

        Ok(dir_proxy)
    }

    fn load_vmo_from_path(&self, path: &Path) -> Result<mem::Buffer, AssetCollectionError> {
        let path_string = path.to_str().unwrap_or_default();
        trace::duration!(
                "fonts",
                "asset:fetcher:load_vmo_from_path",
                "path" => path_string);
        let file = File::open(path)
            .map_err(|e| AssetCollectionError::LocalFileNotAccessible(path.to_owned(), e.into()))?;
        let vmo = fdio::get_vmo_copy_from_file(&file)
            .map_err(|e| AssetCollectionError::LocalFileNotAccessible(path.to_owned(), e.into()))?;
        let size = file
            .metadata()
            .map_err(|e| AssetCollectionError::LocalFileNotAccessible(path.to_owned(), e.into()))?
            .len();
        Ok(mem::Buffer { vmo, size })
    }

    async fn load_vmo_from_directory_proxy(
        &self,
        directory_proxy: io::DirectoryProxy,
        package_locator: &v2::PackageLocator,
        file_name: &str,
    ) -> Result<mem::Buffer, AssetCollectionError> {
        trace::duration!(
            "fonts",
            "asset:collection:load_buffer_from_directory_proxy",
            "file_name" => file_name);

        let packaged_file_error = |cause: Error| AssetCollectionError::PackagedFileError {
            file_name: file_name.to_string(),
            package_locator: package_locator.clone(),
            cause,
        };

        let file_proxy = fuchsia_fs::directory::open_file_no_describe(
            &directory_proxy,
            &file_name,
            io::OpenFlags::RIGHT_READABLE,
        )
        .map_err(Into::into)
        .map_err(|e| packaged_file_error(e))?;

        let vmo = file_proxy
            .get_backing_memory(io::VmoFlags::READ)
            .await
            .map_err(|e| packaged_file_error(e.into()))?
            .map_err(zx::Status::from_raw)
            .map_err(|e| packaged_file_error(e.into()))?;

        let size = vmo.get_content_size().map_err(|e| packaged_file_error(e.into()))?;

        Ok(mem::Buffer { vmo, size })
    }
}
