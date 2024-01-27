// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Test cases which simulate fshost running in the configuration used in recovery builds (which,
//! among other things, sets the fvm_ramdisk flag to prevent binding of the on-disk filesystems.)

use {
    super::{data_fs_name, data_fs_spec, data_fs_type, new_builder},
    fidl_fuchsia_fshost as fshost, fidl_fuchsia_io as fio,
    fshost::{AdminProxy, AdminWriteDataFileResult},
    fuchsia_zircon::{self as zx, HandleBased as _},
};

const PAYLOAD: &[u8] = b"top secret stuff";
const SECRET_FILE_NAME: &'static str = "inconspicuous/secret.txt";
const SMALL_DISK_SIZE: u64 = 25165824;

async fn call_write_data_file(admin: &AdminProxy) -> AdminWriteDataFileResult {
    let vmo = zx::Vmo::create(1024).unwrap();
    vmo.write(PAYLOAD, 0).unwrap();
    vmo.set_content_size(&(PAYLOAD.len() as u64)).unwrap();
    admin
        .write_data_file(SECRET_FILE_NAME, vmo)
        .await
        .expect("write_data_file failed: transport error")
}

#[fuchsia::test]
async fn unformatted() {
    let mut builder = new_builder();
    builder.fshost().set_fvm_ramdisk().set_ramdisk_prefix("/nada/zip/zilch");
    builder.with_disk();
    let fixture = builder.build().await;

    let admin =
        fixture.realm.root.connect_to_protocol_at_exposed_dir::<fshost::AdminMarker>().unwrap();
    call_write_data_file(&admin).await.expect("write_data_file failed");
    let vmo = fixture.ramdisk_vmo().unwrap().duplicate_handle(zx::Rights::SAME_RIGHTS).unwrap();
    fixture.tear_down().await;

    let fixture = new_builder().with_disk_from_vmo(vmo).build().await;
    fixture.check_fs_type("data", data_fs_type()).await;

    let (secret, secret_server) = fidl::endpoints::create_proxy::<fio::NodeMarker>().unwrap();
    fixture
        .dir("data")
        .open(fio::OpenFlags::RIGHT_READABLE, 0, SECRET_FILE_NAME, secret_server)
        .expect("open failed");
    secret.get_attr().await.expect("get_attr failed");

    fixture.tear_down().await;
}

#[fuchsia::test]
async fn unformatted_small_disk() {
    let mut builder = new_builder();
    builder.fshost().set_fvm_ramdisk().set_ramdisk_prefix("/nada/zip/zilch");
    builder.with_disk().size(SMALL_DISK_SIZE).data_volume_size(SMALL_DISK_SIZE / 2);
    let fixture = builder.build().await;

    let admin =
        fixture.realm.root.connect_to_protocol_at_exposed_dir::<fshost::AdminMarker>().unwrap();
    if data_fs_name() == "f2fs" {
        call_write_data_file(&admin).await.expect_err("write_data_file succeeded");
        return;
    }
    call_write_data_file(&admin).await.expect("write_data_file failed");
    let vmo = fixture.ramdisk_vmo().unwrap().duplicate_handle(zx::Rights::SAME_RIGHTS).unwrap();
    fixture.tear_down().await;

    let fixture = new_builder().with_disk_from_vmo(vmo).build().await;

    fixture.check_fs_type("data", data_fs_type()).await;

    let (secret, secret_server) = fidl::endpoints::create_proxy::<fio::NodeMarker>().unwrap();
    fixture
        .dir("data")
        .open(fio::OpenFlags::RIGHT_READABLE, 0, SECRET_FILE_NAME, secret_server)
        .expect("open failed");
    secret.get_attr().await.expect("get_attr failed");

    fixture.tear_down().await;
}

#[fuchsia::test]
async fn formatted() {
    let mut builder = new_builder();
    builder.fshost().set_fvm_ramdisk().set_ramdisk_prefix("/nada/zip/zilch");
    builder.with_disk().format_data(data_fs_spec());
    let fixture = builder.build().await;

    let admin =
        fixture.realm.root.connect_to_protocol_at_exposed_dir::<fshost::AdminMarker>().unwrap();
    call_write_data_file(&admin).await.expect("write_data_file failed");
    let vmo = fixture.ramdisk_vmo().unwrap().duplicate_handle(zx::Rights::SAME_RIGHTS).unwrap();
    fixture.tear_down().await;

    let fixture = new_builder().with_disk_from_vmo(vmo).build().await;

    fixture.check_fs_type("data", data_fs_type()).await;

    let (file, server) = fidl::endpoints::create_proxy::<fio::NodeMarker>().unwrap();
    fixture
        .dir("data")
        .open(fio::OpenFlags::RIGHT_READABLE, 0, "foo", server)
        .expect("open failed");
    file.get_attr().await.expect("get_attr failed");

    let (secret, secret_server) = fidl::endpoints::create_proxy::<fio::NodeMarker>().unwrap();
    fixture
        .dir("data")
        .open(fio::OpenFlags::RIGHT_READABLE, 0, SECRET_FILE_NAME, secret_server)
        .expect("open failed");
    secret.get_attr().await.expect("get_attr failed");

    fixture.tear_down().await;
}
