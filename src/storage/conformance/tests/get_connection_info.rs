// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    fidl_fuchsia_io as fio,
    io_conformance_util::{test_harness::TestHarness, *},
};

/// Verify allowed file operations map to the rights of the connection.
#[fuchsia::test]
async fn get_connection_info_file() {
    let harness = TestHarness::new().await;

    for file_flags in harness.file_rights.valid_combos() {
        let root = root_directory(vec![file(TEST_FILE, vec![])]);
        let test_dir = harness.get_directory(root, harness.dir_rights.all());
        let file = open_file_with_flags(&test_dir, file_flags, TEST_FILE).await;

        // TODO(http://fxbug.dev/77623): Restrict GET_ATTRIBUTES.
        let mut expected_operations = fio::Operations::GET_ATTRIBUTES;
        if file_flags.contains(fio::OpenFlags::RIGHT_READABLE) {
            expected_operations |= fio::Operations::READ_BYTES;
        }
        if file_flags.contains(fio::OpenFlags::RIGHT_WRITABLE) {
            expected_operations |= fio::Operations::WRITE_BYTES;
        }
        if file_flags.contains(fio::OpenFlags::RIGHT_EXECUTABLE) {
            expected_operations |= fio::Operations::EXECUTE;
        }

        assert_eq!(
            file.get_connection_info().await.unwrap(),
            fio::ConnectionInfo { rights: Some(expected_operations), ..fio::ConnectionInfo::EMPTY }
        );
    }
}

/// Verify allowed operations for a node reference connection to a file.
#[fuchsia::test]
async fn get_connection_info_file_node_reference() {
    let harness = TestHarness::new().await;

    let root = root_directory(vec![file(TEST_FILE, vec![])]);
    let test_dir = harness.get_directory(root, harness.dir_rights.all());
    let file = open_file_with_flags(&test_dir, fio::OpenFlags::NODE_REFERENCE, TEST_FILE).await;
    // Node references should only have the ability to get attributes.
    // TODO(http://fxbug.dev/77623): Restrict GET_ATTRIBUTES.
    assert_eq!(
        file.get_connection_info().await.unwrap(),
        fio::ConnectionInfo {
            rights: Some(fio::Operations::GET_ATTRIBUTES),
            ..fio::ConnectionInfo::EMPTY
        }
    );
}

/// Verify allowed operations for a direct connection to a directory.
#[fuchsia::test]
async fn get_connection_info_directory() {
    let harness = TestHarness::new().await;

    for dir_flags in harness.dir_rights.valid_combos() {
        let root = root_directory(vec![directory("dir", vec![])]);
        let test_dir = harness.get_directory(root, harness.dir_rights.all());
        let dir = open_dir_with_flags(&test_dir, dir_flags, "dir").await;

        // TODO(https://fxbug.dev/77623): Restrict GET_ATTRIBUTES, ENUMERATE, and TRAVERSE.
        assert_eq!(
            dir.get_connection_info().await.unwrap(),
            fio::ConnectionInfo {
                rights: Some(
                    fio::Operations::GET_ATTRIBUTES
                        | fio::Operations::ENUMERATE
                        | fio::Operations::TRAVERSE
                ),
                ..fio::ConnectionInfo::EMPTY
            }
        );
    }
}

/// Verify allowed operations for a node reference connection to a directory.
#[fuchsia::test]
async fn get_connection_info_directory_node_reference() {
    let harness = TestHarness::new().await;

    let root = root_directory(vec![directory("dir", vec![])]);
    let test_dir = harness.get_directory(root, harness.dir_rights.all());
    let dir = open_dir_with_flags(&test_dir, fio::OpenFlags::NODE_REFERENCE, "dir").await;
    // Node references should only have the ability to get attributes.
    // TODO(http://fxbug.dev/77623): Restrict GET_ATTRIBUTES.
    assert_eq!(
        dir.get_connection_info().await.unwrap(),
        fio::ConnectionInfo {
            rights: Some(fio::Operations::GET_ATTRIBUTES),
            ..fio::ConnectionInfo::EMPTY
        }
    );
}