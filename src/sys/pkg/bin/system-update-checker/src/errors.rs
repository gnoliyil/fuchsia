// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("reading /pkgfs/system/meta")]
    ReadSystemMeta(#[source] std::io::Error),

    #[error("parsing /pkgfs/system/meta merkle")]
    ParseSystemMeta(#[source] fuchsia_hash::ParseHashError),

    #[error("connecting to PackageResolver")]
    ConnectPackageResolver(#[source] anyhow::Error),

    #[error("connecting to Paver")]
    ConnectPaver(#[source] anyhow::Error),

    #[error("connecting to SpaceManager")]
    ConnectSpaceManager(#[source] anyhow::Error),

    #[error("system-updater component exited with failure")]
    SystemUpdaterFailed,

    #[error("installation ended unexpectedly")]
    InstallationEndedUnexpectedly,

    #[error("reboot FIDL returned error")]
    RebootFailed(#[source] anyhow::Error),

    #[error("update package")]
    UpdatePackage(#[from] UpdatePackage),
}

#[derive(Debug, thiserror::Error)]
pub enum UpdatePackage {
    #[error("creating Directory proxy to resolve the update package")]
    CreateDirectoryProxy(#[source] fidl::Error),

    #[error("fidl error resolving update package")]
    ResolveFidl(#[source] fidl::Error),

    #[error("resolving update package")]
    Resolve(#[source] fidl_fuchsia_pkg_ext::ResolveError),

    #[error("extracting the 'packages' manifest")]
    ExtractPackagesManifest(#[source] update_package::ParsePackageError),

    #[error("could not find system_image/0 in 'packages' manifest")]
    MissingSystemImage,

    #[error("system_image/0 pkg url was not merkle pinned: {0}")]
    UnPinnedSystemImage(fuchsia_url::AbsolutePackageUrl),

    #[error("extracting package hash")]
    Hash(#[source] update_package::HashError),
}
