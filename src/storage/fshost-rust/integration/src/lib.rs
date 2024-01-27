// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    fidl::endpoints::create_proxy,
    fidl_fuchsia_boot as fboot, fidl_fuchsia_feedback as ffeedback, fidl_fuchsia_io as fio,
    fidl_fuchsia_logger as flogger, fidl_fuchsia_process as fprocess,
    fuchsia_component_test::{Capability, ChildOptions, RealmBuilder, RealmInstance, Ref, Route},
    fuchsia_zircon as zx,
    futures::{
        channel::mpsc::{self},
        FutureExt as _, StreamExt as _,
    },
    ramdevice_client::{RamdiskClient, RamdiskClientBuilder},
};

pub mod disk_builder;
pub mod fshost_builder;
mod mocks;

pub struct TestFixtureBuilder {
    netboot: bool,
    no_fuchsia_boot: bool,
    disk: Option<disk_builder::Disk>,
    fshost: fshost_builder::FshostBuilder,
    zbi_ramdisk: Option<disk_builder::DiskBuilder>,
}

impl TestFixtureBuilder {
    pub fn new(fshost_component_name: &'static str) -> Self {
        Self {
            netboot: false,
            no_fuchsia_boot: false,
            disk: None,
            fshost: fshost_builder::FshostBuilder::new(fshost_component_name),
            zbi_ramdisk: None,
        }
    }

    pub fn fshost(&mut self) -> &mut fshost_builder::FshostBuilder {
        &mut self.fshost
    }

    pub fn with_disk(&mut self) -> &mut disk_builder::DiskBuilder {
        self.disk = Some(disk_builder::Disk::Builder(disk_builder::DiskBuilder::new()));
        self.disk.as_mut().unwrap().builder()
    }

    pub fn with_disk_from_vmo(mut self, vmo: zx::Vmo) -> Self {
        self.disk = Some(disk_builder::Disk::Prebuilt(vmo));
        self
    }

    pub fn with_zbi_ramdisk(&mut self) -> &mut disk_builder::DiskBuilder {
        self.zbi_ramdisk = Some(disk_builder::DiskBuilder::new());
        self.zbi_ramdisk.as_mut().unwrap()
    }

    pub fn netboot(mut self) -> Self {
        self.netboot = true;
        self
    }

    pub fn no_fuchsia_boot(mut self) -> Self {
        self.no_fuchsia_boot = true;
        self
    }

    pub async fn build(self) -> TestFixture {
        let builder = RealmBuilder::new().await.unwrap();
        let fshost = self.fshost.build(&builder).await;

        let maybe_zbi_vmo = match self.zbi_ramdisk {
            Some(disk_builder) => Some(disk_builder.build_as_zbi_ramdisk().await),
            None => None,
        };
        let (tx, crash_reports) = mpsc::channel(32);
        let mocks = mocks::new_mocks(self.netboot, maybe_zbi_vmo, tx).await;

        let mocks = builder
            .add_local_child("mocks", move |h| mocks(h).boxed(), ChildOptions::new())
            .await
            .unwrap();
        builder
            .add_route(
                Route::new()
                    .capability(Capability::protocol::<ffeedback::CrashReporterMarker>())
                    .from(&mocks)
                    .to(&fshost),
            )
            .await
            .unwrap();
        if !self.no_fuchsia_boot {
            builder
                .add_route(
                    Route::new()
                        .capability(Capability::protocol::<fboot::ArgumentsMarker>())
                        .capability(Capability::protocol::<fboot::ItemsMarker>())
                        .from(&mocks)
                        .to(&fshost),
                )
                .await
                .unwrap();
        }

        let drivers = builder
            .add_child(
                "storage_driver_test_realm",
                "#meta/storage_driver_test_realm.cm",
                ChildOptions::new().eager(),
            )
            .await
            .unwrap();
        builder
            .add_route(
                Route::new()
                    .capability(Capability::directory("dev-topological").rights(fio::R_STAR_DIR))
                    .from(&drivers)
                    .to(Ref::parent())
                    .to(&fshost),
            )
            .await
            .unwrap();
        builder
            .add_route(
                Route::new()
                    .capability(Capability::protocol::<fprocess::LauncherMarker>())
                    .capability(Capability::protocol::<flogger::LogSinkMarker>())
                    .from(Ref::parent())
                    .to(&drivers),
            )
            .await
            .unwrap();

        let mut fixture = TestFixture {
            realm: builder.build().await.unwrap(),
            ramdisks: Vec::new(),
            ramdisk_vmo: None,
            crash_reports,
        };

        if let Some(disk) = self.disk {
            let vmo = disk.get_vmo().await;
            let vmo_clone =
                vmo.create_child(zx::VmoChildOptions::SLICE, 0, vmo.get_size().unwrap()).unwrap();

            fixture.add_ramdisk(vmo).await;
            fixture.ramdisk_vmo = Some(vmo_clone);
        }

        fixture
    }
}

pub struct TestFixture {
    pub realm: RealmInstance,
    pub ramdisks: Vec<RamdiskClient>,
    pub ramdisk_vmo: Option<zx::Vmo>,
    pub crash_reports: mpsc::Receiver<ffeedback::CrashReport>,
}

impl TestFixture {
    pub async fn tear_down(mut self) {
        self.realm.destroy().await.unwrap();
        assert_eq!(self.crash_reports.next().await, None);
    }

    pub fn dir(&self, dir: &str) -> fio::DirectoryProxy {
        let (dev, server) = create_proxy::<fio::DirectoryMarker>().expect("create_proxy failed");
        self.realm
            .root
            .get_exposed_dir()
            .open(
                fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_WRITABLE,
                fio::ModeType::empty(),
                dir,
                server.into_channel().into(),
            )
            .expect("open failed");
        dev
    }

    pub async fn check_fs_type(&self, dir: &str, fs_type: u32) {
        let (status, info) = self.dir(dir).query_filesystem().await.expect("query failed");
        assert_eq!(zx::Status::from_raw(status), zx::Status::OK);
        assert!(info.is_some());
        assert_eq!(info.unwrap().fs_type, fs_type);
    }

    /// Check for the existence of a well-known test file in the data volume. This file is placed
    /// by the disk builder if it formats the filesystem beforehand.
    pub async fn check_test_data_file(&self) {
        let (file, server) = create_proxy::<fio::NodeMarker>().unwrap();
        self.dir("data")
            .open(fio::OpenFlags::RIGHT_READABLE, fio::ModeType::empty(), "foo", server)
            .expect("open failed");
        file.get_attr().await.expect("get_attr failed");

        let data = self.dir("data");
        fuchsia_fs::directory::open_file(&data, "foo", fio::OpenFlags::RIGHT_READABLE)
            .await
            .unwrap();

        fuchsia_fs::directory::open_directory(&data, "ssh", fio::OpenFlags::RIGHT_READABLE)
            .await
            .unwrap();
        fuchsia_fs::directory::open_directory(&data, "ssh/config", fio::OpenFlags::RIGHT_READABLE)
            .await
            .unwrap();
        fuchsia_fs::directory::open_directory(&data, "problems", fio::OpenFlags::RIGHT_READABLE)
            .await
            .unwrap();

        let authorized_keys = fuchsia_fs::directory::open_file(
            &data,
            "ssh/authorized_keys",
            fio::OpenFlags::RIGHT_READABLE,
        )
        .await
        .unwrap();
        assert_eq!(
            &fuchsia_fs::file::read_to_string(&authorized_keys).await.unwrap(),
            "public key!"
        );
    }

    pub fn ramdisk_vmo(&self) -> Option<&zx::Vmo> {
        self.ramdisk_vmo.as_ref()
    }

    pub async fn add_ramdisk(&mut self, vmo: zx::Vmo) {
        let dev = self.dir("dev-topological");
        let ramdisk =
            RamdiskClientBuilder::new_with_vmo(vmo, Some(512)).dev_root(dev).build().await.unwrap();
        self.ramdisks.push(ramdisk);
    }

    /// This must be called if any crash reports are expected, since spurious reports will cause a
    /// failure in TestFixture::tear_down.
    pub async fn wait_for_crash_reports(
        &mut self,
        count: usize,
        expected_program: &'_ str,
        expected_signature: &'_ str,
    ) {
        for _ in 0..count {
            let report = self.crash_reports.next().await.expect("Sender closed");
            assert_eq!(report.program_name.as_deref(), Some(expected_program));
            assert_eq!(report.crash_signature.as_deref(), Some(expected_signature));
        }
    }
}
