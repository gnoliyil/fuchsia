// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    blobfs_ramdisk::BlobfsRamdisk,
    fidl::endpoints::DiscoverableProtocolMarker as _,
    fidl_fuchsia_boot as fboot, fidl_fuchsia_io as fio, fidl_fuchsia_pkg as fpkg,
    fuchsia_component_test::{Capability, ChildOptions, RealmBuilder, RealmInstance, Ref, Route},
    futures::{
        future::{BoxFuture, FutureExt as _},
        stream::TryStreamExt as _,
    },
    std::sync::Arc,
    vfs::{directory::entry::DirectoryEntry, execution_scope::ExecutionScope},
};

static PKGFS_BOOT_ARG_KEY: &'static str = "zircon.system.pkgfs.cmd";
static PKGFS_BOOT_ARG_VALUE_PREFIX: &'static str = "bin/pkgsvr+";

trait BootArgumentsStreamHandler: Send + Sync {
    fn handle_stream(&self, stream: fboot::ArgumentsRequestStream) -> BoxFuture<'static, ()>;
}

struct TestEnvBuilder {
    blobfs: Option<BlobfsRamdisk>,
    boot_args: Option<Arc<dyn BootArgumentsStreamHandler>>,
}

impl TestEnvBuilder {
    fn new() -> Self {
        Self { blobfs: None, boot_args: None }
    }

    async fn static_packages(self, static_packages: &[&fuchsia_pkg_testing::Package]) -> Self {
        assert!(self.blobfs.is_none());
        assert!(self.boot_args.is_none());

        let system_image = fuchsia_pkg_testing::SystemImageBuilder::new()
            .static_packages(static_packages)
            .build()
            .await;

        let blobfs = BlobfsRamdisk::start().await.unwrap();
        let root_dir = blobfs.root_dir().unwrap();
        let () = system_image.write_to_blobfs_dir(&root_dir);
        for pkg in static_packages {
            let () = pkg.write_to_blobfs_dir(&root_dir);
        }

        Self {
            blobfs: Some(blobfs),
            boot_args: Some(Arc::new(BootArgsFixedHash::new(*system_image.meta_far_merkle_root()))),
        }
    }

    async fn build(self) -> TestEnv {
        let blobfs = self.blobfs.unwrap();
        let blobfs_dir = vfs::remote::remote_dir(blobfs.root_dir_proxy().unwrap());
        let boot_args = self.boot_args.unwrap();

        let builder = RealmBuilder::new().await.unwrap();

        let resolver = builder
            .add_child("resolver", "#meta/true-base-resolver.cm", ChildOptions::new())
            .await
            .unwrap();

        let local_mocks = builder
            .add_local_child(
                "local_mocks",
                move |handles| {
                    let blobfs_dir = blobfs_dir.clone();
                    let boot_args_clone = boot_args.clone();
                    let out_dir = vfs::pseudo_directory! {
                        "blob" => blobfs_dir,
                        "svc" => vfs::pseudo_directory! {
                            fboot::ArgumentsMarker::PROTOCOL_NAME =>
                                vfs::service::host(move |stream|
                                    boot_args_clone.handle_stream(stream)
                                ),
                        },
                    };
                    let scope = ExecutionScope::new();
                    let () = out_dir.open(
                        scope.clone(),
                        fio::OpenFlags::RIGHT_READABLE
                            | fio::OpenFlags::RIGHT_WRITABLE
                            | fio::OpenFlags::RIGHT_EXECUTABLE,
                        0,
                        vfs::path::Path::dot(),
                        handles.outgoing_dir.into_channel().into(),
                    );
                    async move { Ok(scope.wait().await) }.boxed()
                },
                ChildOptions::new(),
            )
            .await
            .unwrap();

        builder
            .add_route(
                Route::new()
                    .capability(
                        Capability::directory("blob-exec").path("/blob").rights(fio::RX_STAR_DIR),
                    )
                    .capability(Capability::protocol::<fboot::ArgumentsMarker>())
                    .from(&local_mocks)
                    .to(&resolver),
            )
            .await
            .unwrap();

        builder
            .add_route(
                Route::new()
                    .capability(Capability::protocol::<fidl_fuchsia_logger::LogSinkMarker>())
                    .from(Ref::parent())
                    .to(&resolver),
            )
            .await
            .unwrap();

        builder
            .add_route(
                Route::new()
                    .capability(Capability::protocol::<fpkg::PackageResolverMarker>())
                    .from(&resolver)
                    .to(Ref::parent()),
            )
            .await
            .unwrap();

        TestEnv { realm_instance: builder.build().await.unwrap(), _blobfs: blobfs }
    }
}

struct TestEnv {
    realm_instance: RealmInstance,
    _blobfs: BlobfsRamdisk,
}

impl TestEnv {
    async fn resolve_package(
        &self,
        url: &str,
    ) -> Result<(fio::DirectoryProxy, fpkg::ResolutionContext), fpkg::ResolveError> {
        let package_resolver = self
            .realm_instance
            .root
            .connect_to_protocol_at_exposed_dir::<fpkg::PackageResolverMarker>()
            .unwrap();
        let (package, package_server_end) = fidl::endpoints::create_proxy().unwrap();
        let context = package_resolver.resolve(url, package_server_end).await.unwrap()?;
        Ok((package, context))
    }
}

// Responds to requests for "zircon.system.pkgfs.cmd" with the provided hash.
struct BootArgsFixedHash {
    hash: fuchsia_hash::Hash,
}

impl BootArgsFixedHash {
    fn new(hash: fuchsia_hash::Hash) -> Self {
        Self { hash }
    }
}

impl BootArgumentsStreamHandler for BootArgsFixedHash {
    fn handle_stream(&self, mut stream: fboot::ArgumentsRequestStream) -> BoxFuture<'static, ()> {
        let hash = self.hash;
        async move {
            while let Some(request) = stream.try_next().await.unwrap() {
                match request {
                    fboot::ArgumentsRequest::GetString { key, responder } => {
                        assert_eq!(key, PKGFS_BOOT_ARG_KEY);
                        responder
                            .send(Some(&format!("{}{}", PKGFS_BOOT_ARG_VALUE_PREFIX, hash)))
                            .unwrap();
                    }
                    req => panic!("unexpected request {:?}", req),
                }
            }
        }
        .boxed()
    }
}

#[fuchsia::test]
async fn resolve_static_package() {
    let base_pkg =
        fuchsia_pkg_testing::PackageBuilder::new("a-base-package").build().await.unwrap();
    let env = TestEnvBuilder::new().static_packages(&[&base_pkg]).await.build().await;

    let (resolved, context) =
        env.resolve_package("fuchsia-pkg://fuchsia.com/a-base-package/0").await.unwrap();

    assert_eq!(context, fpkg::ResolutionContext { bytes: vec![] });
    let () = base_pkg.verify_contents(&resolved).await.unwrap();
}

#[fuchsia::test]
async fn resolve_system_image() {
    let env = TestEnvBuilder::new().static_packages(&[]).await.build().await;

    let (resolved, _) =
        env.resolve_package("fuchsia-pkg://fuchsia.com/system_image/0").await.unwrap();

    assert_eq!(
        fuchsia_pkg::PackageDirectory::from_proxy(resolved)
            .meta_package()
            .await
            .unwrap()
            .into_path(),
        system_image::SystemImage::package_path()
    );
}
