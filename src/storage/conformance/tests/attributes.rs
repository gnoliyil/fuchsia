// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    fidl_fuchsia_io as fio, fuchsia_zircon as zx,
    io_conformance_util::{test_harness::TestHarness, *},
};

#[fuchsia::test]
async fn set_attr_file_with_sufficient_rights() {
    let harness = TestHarness::new().await;
    if !harness.config.supports_set_attr.unwrap_or_default() {
        return;
    }

    for dir_flags in harness.file_rights.valid_combos_with(fio::OpenFlags::RIGHT_WRITABLE) {
        let root = root_directory(vec![file("file", vec![])]);
        let test_dir = harness.get_directory(root, harness.dir_rights.all());
        let file = open_file_with_flags(&test_dir, dir_flags, "file").await;

        let (status, old_attr) = file.get_attr().await.expect("get_attr failed");
        assert_eq!(zx::Status::from_raw(status), zx::Status::OK);

        // Set CREATION_TIME flag, but not MODIFICATION_TIME.
        let status = file
            .set_attr(
                fio::NodeAttributeFlags::CREATION_TIME,
                &fio::NodeAttributes {
                    creation_time: 111,
                    modification_time: 222,
                    ..EMPTY_NODE_ATTRS
                },
            )
            .await
            .expect("set_attr failed");
        assert_eq!(zx::Status::from_raw(status), zx::Status::OK);

        let (status, new_attr) = file.get_attr().await.expect("get_attr failed");
        assert_eq!(zx::Status::from_raw(status), zx::Status::OK);
        // Check that only creation_time was updated.
        let expected = fio::NodeAttributes { creation_time: 111, ..old_attr };
        assert_eq!(new_attr, expected);
    }
}

#[fuchsia::test]
async fn set_attr_file_with_insufficient_rights() {
    let harness = TestHarness::new().await;
    if !harness.config.supports_set_attr.unwrap_or_default() {
        return;
    }

    for dir_flags in harness.file_rights.valid_combos_without(fio::OpenFlags::RIGHT_WRITABLE) {
        let root = root_directory(vec![file("file", vec![])]);
        let test_dir = harness.get_directory(root, harness.dir_rights.all());
        let file = open_file_with_flags(&test_dir, dir_flags, "file").await;

        let status = file
            .set_attr(
                fio::NodeAttributeFlags::CREATION_TIME,
                &fio::NodeAttributes {
                    creation_time: 111,
                    modification_time: 222,
                    ..EMPTY_NODE_ATTRS
                },
            )
            .await
            .expect("set_attr failed");
        assert_eq!(zx::Status::from_raw(status), zx::Status::BAD_HANDLE);
    }
}

#[fuchsia::test]
async fn set_attr_directory_with_sufficient_rights() {
    let harness = TestHarness::new().await;
    if !harness.config.supports_set_attr.unwrap_or_default() {
        return;
    }

    for dir_flags in harness.file_rights.valid_combos_with(fio::OpenFlags::RIGHT_WRITABLE) {
        let root = root_directory(vec![directory("dir", vec![])]);
        let test_dir = harness.get_directory(root, harness.dir_rights.all());
        let dir = open_dir_with_flags(&test_dir, dir_flags, "dir").await;

        let (status, old_attr) = dir.get_attr().await.expect("get_attr failed");
        assert_eq!(zx::Status::from_raw(status), zx::Status::OK);

        // Set CREATION_TIME flag, but not MODIFICATION_TIME.
        let status = dir
            .set_attr(
                fio::NodeAttributeFlags::CREATION_TIME,
                &fio::NodeAttributes {
                    creation_time: 111,
                    modification_time: 222,
                    ..EMPTY_NODE_ATTRS
                },
            )
            .await
            .expect("set_attr failed");
        assert_eq!(zx::Status::from_raw(status), zx::Status::OK);

        let (status, new_attr) = dir.get_attr().await.expect("get_attr failed");
        assert_eq!(zx::Status::from_raw(status), zx::Status::OK);
        // Check that only creation_time was updated.
        let expected = fio::NodeAttributes { creation_time: 111, ..old_attr };
        assert_eq!(new_attr, expected);
    }
}

#[fuchsia::test]
async fn set_attr_directory_with_insufficient_rights() {
    let harness = TestHarness::new().await;
    if !harness.config.supports_set_attr.unwrap_or_default() {
        return;
    }

    for dir_flags in harness.file_rights.valid_combos_without(fio::OpenFlags::RIGHT_WRITABLE) {
        let root = root_directory(vec![directory("dir", vec![])]);
        let test_dir = harness.get_directory(root, harness.dir_rights.all());
        let dir = open_dir_with_flags(&test_dir, dir_flags, "dir").await;

        let status = dir
            .set_attr(
                fio::NodeAttributeFlags::CREATION_TIME,
                &fio::NodeAttributes {
                    creation_time: 111,
                    modification_time: 222,
                    ..EMPTY_NODE_ATTRS
                },
            )
            .await
            .expect("set_attr failed");
        assert_eq!(zx::Status::from_raw(status), zx::Status::BAD_HANDLE);
    }
}

#[fuchsia::test]
async fn get_update_attributes_file_unsupported() {
    let harness = TestHarness::new().await;

    let root = root_directory(vec![file(TEST_FILE, vec![])]);
    let test_dir = harness.get_directory(root, harness.dir_rights.all());
    let file_proxy =
        open_file_with_flags(&test_dir, fio::OpenFlags::RIGHT_READABLE, TEST_FILE).await;

    // fuchsia.io/Node.GetAttributes
    assert_eq!(
        file_proxy.get_attributes(fio::NodeAttributesQuery::empty()).await.unwrap(),
        Err(zx::Status::NOT_SUPPORTED.into_raw())
    );

    // fuchsia.io/Node.UpdateAttributes
    assert_eq!(
        file_proxy.update_attributes(&fio::MutableNodeAttributes::default()).await.unwrap(),
        Err(zx::Status::NOT_SUPPORTED.into_raw())
    );
}

#[fuchsia::test]
async fn get_update_attributes_file_node_reference_unsupported() {
    let harness = TestHarness::new().await;

    let root = root_directory(vec![file(TEST_FILE, vec![])]);
    let test_dir = harness.get_directory(root, harness.dir_rights.all());
    let file_proxy =
        open_file_with_flags(&test_dir, fio::OpenFlags::NODE_REFERENCE, TEST_FILE).await;

    // fuchsia.io/Node.GetAttributes
    assert_eq!(
        file_proxy.get_attributes(fio::NodeAttributesQuery::empty()).await.unwrap(),
        Err(zx::Status::NOT_SUPPORTED.into_raw())
    );

    // fuchsia.io/Node.UpdateAttributes
    assert_eq!(
        file_proxy.update_attributes(&fio::MutableNodeAttributes::default()).await.unwrap(),
        Err(zx::Status::NOT_SUPPORTED.into_raw())
    );
}

#[fuchsia::test]
async fn get_update_attributes_directory_unsupported() {
    let harness = TestHarness::new().await;

    let root = root_directory(vec![directory("dir", vec![])]);
    let test_dir = harness.get_directory(root, harness.dir_rights.all());
    let dir_proxy = open_dir_with_flags(&test_dir, fio::OpenFlags::RIGHT_READABLE, "dir").await;

    // fuchsia.io/Node.GetAttributes
    assert_eq!(
        dir_proxy.get_attributes(fio::NodeAttributesQuery::empty()).await.unwrap(),
        Err(zx::Status::NOT_SUPPORTED.into_raw())
    );

    // fuchsia.io/Node.UpdateAttributes
    assert_eq!(
        dir_proxy.update_attributes(&fio::MutableNodeAttributes::default()).await.unwrap(),
        Err(zx::Status::NOT_SUPPORTED.into_raw())
    );
}

#[fuchsia::test]
async fn get_update_attributes_directory_node_reference_unsupported() {
    let harness = TestHarness::new().await;

    let root = root_directory(vec![directory("dir", vec![])]);
    let test_dir = harness.get_directory(root, harness.dir_rights.all());
    let dir_proxy = open_dir_with_flags(&test_dir, fio::OpenFlags::NODE_REFERENCE, "dir").await;

    // fuchsia.io/Node.GetAttributes
    assert_eq!(
        dir_proxy.get_attributes(fio::NodeAttributesQuery::empty()).await.unwrap(),
        Err(zx::Status::NOT_SUPPORTED.into_raw())
    );

    // fuchsia.io/Node.UpdateAttributes
    assert_eq!(
        dir_proxy.update_attributes(&fio::MutableNodeAttributes::default()).await.unwrap(),
        Err(zx::Status::NOT_SUPPORTED.into_raw())
    );
}
