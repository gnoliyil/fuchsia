// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{self, Context as _},
    fidl::endpoints::ServerEnd,
    fidl_fuchsia_boot as fboot, fidl_fuchsia_io as fio, fidl_fuchsia_pkg as fpkg,
    futures::{
        future::TryFutureExt as _,
        stream::{StreamExt as _, TryStreamExt as _},
    },
    std::collections::HashMap,
    tracing::error,
};

pub(crate) async fn main() -> anyhow::Result<()> {
    tracing::info!("started");

    let blobfs =
        blobfs::Client::open_from_namespace_executable().context("failed to open /blob")?;
    let base_packages = determine_base_packages(
        &blobfs,
        &fuchsia_component::client::connect_to_protocol::<fboot::ArgumentsMarker>()
            .context("failed to connect to fuchsia.boot/Arguments")?,
    )
    .await
    .context("determine base packages")?;

    let mut service_fs = fuchsia_component::server::ServiceFs::new_local();
    service_fs.dir("svc").add_fidl_service(Services::PackageResolver);
    service_fs.take_and_serve_directory_handle().context("failed to serve outgoing namespace")?;
    let () = service_fs
        .for_each_concurrent(None, |request| async {
            match request {
                Services::PackageResolver(stream) => {
                    serve_package_request_stream(stream, &base_packages, &blobfs)
                        .unwrap_or_else(|e| {
                            error!("failed to serve package resolver request: {:#}", e)
                        })
                        .await
                }
            }
        })
        .await;

    Ok(())
}

async fn determine_base_packages(
    blobfs: &blobfs::Client,
    boot_args: &fboot::ArgumentsProxy,
) -> anyhow::Result<HashMap<fuchsia_url::UnpinnedAbsolutePackageUrl, fuchsia_hash::Hash>> {
    let system_image = system_image::SystemImage::new(blobfs.clone(), boot_args)
        .await
        .context("failed to load system_image package")?;
    let static_packages =
        system_image.static_packages().await.context("failed to determine static packages")?;
    let base_repo = fuchsia_url::RepositoryUrl::parse_host("fuchsia.com".into())
        .expect("valid repository hostname");
    Ok(HashMap::from_iter(
        static_packages
            .into_contents()
            .chain([(system_image::SystemImage::package_path(), *system_image.hash())])
            .map(|(path, hash)| {
                let (name, variant) = path.into_name_and_variant();
                (
                    fuchsia_url::UnpinnedAbsolutePackageUrl::new(
                        base_repo.clone(),
                        name,
                        Some(variant),
                    ),
                    hash,
                )
            }),
    ))
}

enum Services {
    PackageResolver(fpkg::PackageResolverRequestStream),
}

async fn serve_package_request_stream(
    mut stream: fpkg::PackageResolverRequestStream,
    base_packages: &HashMap<fuchsia_url::UnpinnedAbsolutePackageUrl, fuchsia_hash::Hash>,
    blobfs: &blobfs::Client,
) -> anyhow::Result<()> {
    while let Some(request) =
        stream.try_next().await.context("failed to read request from FIDL stream")?
    {
        match request {
            fpkg::PackageResolverRequest::Resolve { package_url, dir, responder } => {
                let () = responder
                    .send(
                        &mut resolve_package(&package_url, dir, base_packages, blobfs)
                            .await
                            .map_err(|e| {
                                let fidl_err = (&e).into();
                                error!(
                                    "failed to resolve package {}: {:#}",
                                    package_url,
                                    anyhow::anyhow!(e)
                                );
                                fidl_err
                            }),
                    )
                    .context("sending fuchsia.pkg/PackageResolver.Resolve response")?;
            }
            fpkg::PackageResolverRequest::ResolveWithContext {
                package_url,
                context: _,
                dir: _,
                responder,
            } => {
                error!(
                    "unsupported fuchsia.pkg/PackageResolver.ResolveWithContext called with {:?}",
                    package_url
                );
                let () = responder
                    .send(&mut Err(fpkg::ResolveError::Internal))
                    .context("sending fuchsia.pkg/PackageResolver.ResolveWithContext response")?;
            }
            fpkg::PackageResolverRequest::GetHash { package_url, responder } => {
                error!(
                    "unsupported fuchsia.pkg/PackageResolver.GetHash called with {:?}",
                    package_url
                );
                let () = responder
                    .send(&mut Err(fuchsia_zircon::Status::NOT_SUPPORTED.into_raw()))
                    .context("sending fuchsia.pkg/PackageResolver.GetHash response")?;
            }
        }
    }
    Ok(())
}

async fn resolve_package(
    package_url: &str,
    dir: ServerEnd<fio::DirectoryMarker>,
    base_packages: &HashMap<fuchsia_url::UnpinnedAbsolutePackageUrl, fuchsia_hash::Hash>,
    blobfs: &blobfs::Client,
) -> Result<fpkg::ResolutionContext, crate::ResolverError> {
    let package_url = package_url.parse()?;
    let hash = base_packages
        .get(&package_url)
        .ok_or_else(|| crate::ResolverError::PackageNotInBase(package_url.clone().into()))?;

    let () = package_directory::serve(
        package_directory::ExecutionScope::new(),
        blobfs.clone(),
        *hash,
        fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_EXECUTABLE,
        dir,
    )
    .await
    .map_err(crate::ResolverError::ServePackageDirectory)?;
    Ok(fpkg::ResolutionContext { bytes: vec![] })
}

#[cfg(test)]
mod tests {
    use {super::*, assert_matches::assert_matches};

    #[fuchsia::test]
    async fn reject_pinned_url() {
        assert_matches!(
            resolve_package(
                "fuchsia-pkg://fuchsia.com/name?\
                    hash=0000000000000000000000000000000000000000000000000000000000000000",
                fidl::endpoints::create_endpoints().unwrap().1,
                &HashMap::new(),
                &blobfs::Client::new_test().0
            )
            .await,
            Err(crate::ResolverError::InvalidUrl(fuchsia_url::ParseError::CannotContainHash))
        )
    }
}
