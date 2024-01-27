// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    fidl_fuchsia_io as fio, fuchsia_zircon as zx,
    io_conformance_util::{test_harness::TestHarness, *},
};

#[fuchsia::test]
async fn file_read_with_sufficient_rights() {
    let harness = TestHarness::new().await;

    for file_flags in harness
        .file_rights
        .valid_combos_with(fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::NOT_DIRECTORY)
    {
        let root = root_directory(vec![file(TEST_FILE, vec![])]);
        let test_dir = harness.get_directory(root, harness.dir_rights.all());

        let file = open_node::<fio::FileMarker>(&test_dir, file_flags, TEST_FILE).await;
        let _data: Vec<u8> = file
            .read(0)
            .await
            .expect("read failed")
            .map_err(zx::Status::from_raw)
            .expect("read error");
    }
}

#[fuchsia::test]
async fn file_read_with_insufficient_rights() {
    let harness = TestHarness::new().await;

    for file_flags in harness
        .file_rights
        .valid_combos_without(fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::NOT_DIRECTORY)
    {
        let root = root_directory(vec![file(TEST_FILE, vec![])]);
        let test_dir = harness.get_directory(root, harness.dir_rights.all());

        let file = open_node::<fio::FileMarker>(&test_dir, file_flags, TEST_FILE).await;
        let result = file.read(0).await.expect("read failed").map_err(zx::Status::from_raw);
        assert_eq!(result, Err(zx::Status::BAD_HANDLE))
    }
}

#[fuchsia::test]
async fn file_read_with_max_transfer() {
    let harness = TestHarness::new().await;

    for file_flags in harness
        .file_rights
        .valid_combos_with(fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::NOT_DIRECTORY)
    {
        let contents = vec![0u8; fio::MAX_TRANSFER_SIZE as usize];
        let root = root_directory(vec![file(TEST_FILE, contents)]);
        let test_dir = harness.get_directory(root, harness.dir_rights.all());

        let file = open_node::<fio::FileMarker>(&test_dir, file_flags, TEST_FILE).await;

        let len = file
            .read(fio::MAX_TRANSFER_SIZE)
            .await
            .expect("read failed")
            .map_err(zx::Status::from_raw)
            .expect("read error")
            .len();
        assert_eq!(len, fio::MAX_TRANSFER_SIZE as usize);
    }
}

#[fuchsia::test]
async fn file_read_over_max_transfer() {
    let harness = TestHarness::new().await;

    for file_flags in harness
        .file_rights
        .valid_combos_with(fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::NOT_DIRECTORY)
    {
        let contents = vec![0u8; fio::MAX_TRANSFER_SIZE as usize + 1];
        let root = root_directory(vec![file(TEST_FILE, contents)]);
        let test_dir = harness.get_directory(root, harness.dir_rights.all());

        let file = open_node::<fio::FileMarker>(&test_dir, file_flags, TEST_FILE).await;

        let result = file
            .read(fio::MAX_TRANSFER_SIZE + 1)
            .await
            .expect("read failed")
            .map_err(zx::Status::from_raw);
        assert_eq!(result, Err(zx::Status::OUT_OF_RANGE))
    }
}

#[fuchsia::test]
async fn file_read_at_with_sufficient_rights() {
    let harness = TestHarness::new().await;

    for file_flags in harness
        .file_rights
        .valid_combos_with(fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::NOT_DIRECTORY)
    {
        let root = root_directory(vec![file(TEST_FILE, vec![])]);
        let test_dir = harness.get_directory(root, harness.dir_rights.all());

        let file = open_node::<fio::FileMarker>(&test_dir, file_flags, TEST_FILE).await;
        let _: Vec<u8> = file
            .read_at(0, 0)
            .await
            .expect("read_at failed")
            .map_err(zx::Status::from_raw)
            .expect("read_at error");
    }
}

#[fuchsia::test]
async fn file_read_at_with_insufficient_rights() {
    let harness = TestHarness::new().await;

    for file_flags in harness
        .file_rights
        .valid_combos_without(fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::NOT_DIRECTORY)
    {
        let root = root_directory(vec![file(TEST_FILE, vec![])]);
        let test_dir = harness.get_directory(root, harness.dir_rights.all());

        let file = open_node::<fio::FileMarker>(&test_dir, file_flags, TEST_FILE).await;
        let result =
            file.read_at(0, 0).await.expect("read_at failed").map_err(zx::Status::from_raw);
        assert_eq!(result, Err(zx::Status::BAD_HANDLE))
    }
}

#[fuchsia::test]
async fn file_read_at_with_max_transfer() {
    let harness = TestHarness::new().await;

    for file_flags in harness
        .file_rights
        .valid_combos_with(fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::NOT_DIRECTORY)
    {
        let contents = vec![0u8; fio::MAX_TRANSFER_SIZE as usize];
        let root = root_directory(vec![file(TEST_FILE, contents)]);
        let test_dir = harness.get_directory(root, harness.dir_rights.all());

        let file = open_node::<fio::FileMarker>(&test_dir, file_flags, TEST_FILE).await;

        let len = file
            .read_at(fio::MAX_TRANSFER_SIZE, 0)
            .await
            .expect("read_at failed")
            .map_err(zx::Status::from_raw)
            .expect("read_at error")
            .len();
        assert_eq!(len, fio::MAX_TRANSFER_SIZE as usize)
    }
}

#[fuchsia::test]
async fn file_read_at_over_max_transfer() {
    let harness = TestHarness::new().await;

    for file_flags in harness
        .file_rights
        .valid_combos_with(fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::NOT_DIRECTORY)
    {
        let contents = vec![0u8; fio::MAX_TRANSFER_SIZE as usize + 1];
        let root = root_directory(vec![file(TEST_FILE, contents)]);
        let test_dir = harness.get_directory(root, harness.dir_rights.all());

        let file = open_node::<fio::FileMarker>(&test_dir, file_flags, TEST_FILE).await;

        let result = file
            .read_at(fio::MAX_TRANSFER_SIZE + 1, 0)
            .await
            .expect("read_at failed")
            .map_err(zx::Status::from_raw);
        assert_eq!(result, Err(zx::Status::OUT_OF_RANGE))
    }
}

#[fuchsia::test]
async fn file_read_in_subdirectory() {
    let harness = TestHarness::new().await;

    for file_flags in harness
        .file_rights
        .valid_combos_with(fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::NOT_DIRECTORY)
    {
        let root = root_directory(vec![directory("subdir", vec![file("testing.txt", vec![])])]);
        let test_dir = harness.get_directory(root, harness.dir_rights.all());

        let file = open_node::<fio::FileMarker>(&test_dir, file_flags, "subdir/testing.txt").await;
        let _data: Vec<u8> = file
            .read(0)
            .await
            .expect("read failed")
            .map_err(zx::Status::from_raw)
            .expect("read error");
    }
}
