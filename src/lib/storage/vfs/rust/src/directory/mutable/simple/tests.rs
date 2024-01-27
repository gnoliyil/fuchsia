// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Tests for the mutable simple directory.
//!
//! As both mutable and immutable simple directories share the implementation, there is very little
//! chance that the use cases covered by the unit tests for the immutable simple directory will
//! fail for the mutable case.  So, this suite focuses on the mutable test cases.

use super::simple;

// Macros are exported into the root of the crate.
use crate::{
    assert_close, assert_event, assert_get_token, assert_get_token_err, assert_link_err,
    assert_read, assert_read_dirents, assert_rename, assert_rename_err, assert_unlink,
    assert_unlink_err, assert_watch, assert_watcher_one_message_watched_events,
    open_as_directory_assert_err, open_as_file_assert_err, open_get_directory_proxy_assert_ok,
    open_get_proxy_assert, open_get_vmo_file_proxy_assert_ok,
};

use crate::{
    directory::{
        mutable::simple::tree_constructor,
        test_utils::{run_server_client, test_server_client, DirentsSameInodeBuilder},
    },
    file::vmo::read_only_static,
};

use {
    fidl::Event,
    fidl_fuchsia_io as fio,
    std::sync::{
        atomic::{AtomicU8, Ordering},
        Arc,
    },
    vfs_macros::mut_pseudo_directory,
};

#[test]
fn empty_directory() {
    run_server_client(fio::OpenFlags::RIGHT_READABLE, simple(), |proxy| async move {
        assert_close!(proxy);
    });
}

#[test]
fn unlink_entry() {
    let root = mut_pseudo_directory! {
        "fstab" => read_only_static(b"/dev/fs /"),
        "passwd" => read_only_static(b"[redacted]"),
    };

    run_server_client(
        fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_WRITABLE,
        root,
        |proxy| async move {
            let ro_flags = fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::DESCRIBE;

            open_as_vmo_file_assert_content!(&proxy, ro_flags, "fstab", "/dev/fs /");
            open_as_vmo_file_assert_content!(&proxy, ro_flags, "passwd", "[redacted]");

            assert_unlink!(&proxy, "passwd");

            open_as_vmo_file_assert_content!(&proxy, ro_flags, "fstab", "/dev/fs /");
            open_as_file_assert_err!(&proxy, ro_flags, "passwd", Status::NOT_FOUND);

            assert_close!(proxy);
        },
    );
}

#[test]
fn unlink_absent_entry() {
    let root = mut_pseudo_directory! {
        "fstab" => read_only_static(b"/dev/fs /"),
    };

    run_server_client(
        fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_WRITABLE,
        root,
        |proxy| async move {
            let ro_flags = fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::DESCRIBE;

            open_as_vmo_file_assert_content!(&proxy, ro_flags, "fstab", "/dev/fs /");

            assert_unlink_err!(&proxy, "fstab.2", Status::NOT_FOUND);

            open_as_vmo_file_assert_content!(&proxy, ro_flags, "fstab", "/dev/fs /");

            assert_close!(proxy);
        },
    );
}

#[test]
fn unlink_does_not_traverse() {
    let root = mut_pseudo_directory! {
        "etc" => mut_pseudo_directory! {
            "fstab" => read_only_static(b"/dev/fs /"),
        },
    };

    run_server_client(
        fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_WRITABLE,
        root,
        |proxy| async move {
            let ro_flags = fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::DESCRIBE;

            open_as_vmo_file_assert_content!(&proxy, ro_flags, "etc/fstab", "/dev/fs /");

            assert_unlink_err!(&proxy, "etc/fstab", Status::INVALID_ARGS);

            open_as_vmo_file_assert_content!(&proxy, ro_flags, "etc/fstab", "/dev/fs /");

            assert_close!(proxy);
        },
    );
}

#[test]
fn unlink_fails_for_read_only_source() {
    let root = mut_pseudo_directory! {
        "etc" => mut_pseudo_directory! {
            "fstab" => read_only_static(b"/dev/fs /"),
        },
    };

    test_server_client(
        fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_WRITABLE,
        root,
        |proxy| async move {
            let ro_flags = fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::DESCRIBE;

            let etc = open_get_directory_proxy_assert_ok!(&proxy, ro_flags, "etc");

            assert_unlink_err!(&etc, "fstab", Status::BAD_HANDLE);

            assert_close!(etc);
            assert_close!(proxy);
        },
    )
    .run();
}

/// Keep this test in sync with [`rename_within_directory_with_watchers`].  See there for details.
#[test]
fn rename_within_directory() {
    let root = mut_pseudo_directory! {
        "passwd" => read_only_static(b"/dev/fs /"),
    };

    test_server_client(
        fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_WRITABLE,
        root,
        |proxy| async move {
            let ro_flags = fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::DESCRIBE;

            open_as_vmo_file_assert_content!(&proxy, ro_flags, "passwd", "/dev/fs /");

            let mut root_token = assert_get_token!(&proxy);
            // This should return an error because the source file does not exist
            assert_rename_err!(
                &proxy,
                "file-does-not-exist",
                Event::from(root_token),
                "file-will-not-exist",
                Status::NOT_FOUND
            );
            root_token = assert_get_token!(&proxy);
            assert_rename!(&proxy, "passwd", Event::from(root_token), "fstab");

            open_as_file_assert_err!(&proxy, ro_flags, "passwd", Status::NOT_FOUND);
            open_as_vmo_file_assert_content!(&proxy, ro_flags, "fstab", "/dev/fs /");

            assert_close!(proxy);
        },
    )
    .run();
}

/// Keep this test in sync with [`rename_across_directories_with_watchers`].  See there for
/// details.
#[test]
fn rename_across_directories() {
    let root = mut_pseudo_directory! {
        "tmp" => mut_pseudo_directory! {
            "fstab.new" => read_only_static(b"/dev/fs /"),
        },
        "etc" => mut_pseudo_directory! {},
    };

    test_server_client(
        fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_WRITABLE,
        root,
        |proxy| async move {
            let ro_flags = fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::DESCRIBE;
            let rw_flags = fio::OpenFlags::RIGHT_READABLE
                | fio::OpenFlags::RIGHT_WRITABLE
                | fio::OpenFlags::DESCRIBE;

            open_as_vmo_file_assert_content!(&proxy, ro_flags, "tmp/fstab.new", "/dev/fs /");
            open_as_file_assert_err!(&proxy, ro_flags, "etc/fstab", Status::NOT_FOUND);

            let tmp = open_get_directory_proxy_assert_ok!(&proxy, rw_flags, "tmp");
            let etc = open_get_directory_proxy_assert_ok!(&proxy, rw_flags, "etc");

            let etc_token = assert_get_token!(&etc);

            assert_rename!(&tmp, "fstab.new", Event::from(etc_token), "fstab");

            open_as_file_assert_err!(&proxy, ro_flags, "tmp/fstab.new", Status::NOT_FOUND);
            open_as_vmo_file_assert_content!(&proxy, ro_flags, "etc/fstab", "/dev/fs /");

            assert_close!(etc);
            assert_close!(tmp);
            assert_close!(proxy);
        },
    )
    .run();
}

/// As the renaming code is different depending on the relative order of the directory nodes in
/// memory we need to test both directions.  But as the relative order will change on every run
/// depending on the allocation addresses, one way to test both directions is to rename back and
/// forth.
///
/// Keep this test in sync with [`rename_across_directories_twice_with_watchers`].  See there for
/// details.
#[test]
fn rename_across_directories_twice() {
    let root = mut_pseudo_directory! {
        "etc" => mut_pseudo_directory! {
            "fstab" => read_only_static(b"/dev/fs /"),
        },
        "tmp" => mut_pseudo_directory! {},
    };

    test_server_client(
        fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_WRITABLE,
        root,
        |proxy| async move {
            let ro_flags = fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::DESCRIBE;
            let rw_flags = fio::OpenFlags::RIGHT_READABLE
                | fio::OpenFlags::RIGHT_WRITABLE
                | fio::OpenFlags::DESCRIBE;

            open_as_vmo_file_assert_content!(&proxy, ro_flags, "etc/fstab", "/dev/fs /");
            open_as_file_assert_err!(&proxy, ro_flags, "tmp/fstab.to-edit", Status::NOT_FOUND);

            let etc = open_get_directory_proxy_assert_ok!(&proxy, rw_flags, "etc");
            let tmp = open_get_directory_proxy_assert_ok!(&proxy, rw_flags, "tmp");

            let etc_token = assert_get_token!(&etc);
            let tmp_token = assert_get_token!(&tmp);

            assert_rename!(&etc, "fstab", Event::from(tmp_token), "fstab.to-edit");

            open_as_file_assert_err!(&proxy, ro_flags, "etc/fstab", Status::NOT_FOUND);
            open_as_vmo_file_assert_content!(&proxy, ro_flags, "tmp/fstab.to-edit", "/dev/fs /");

            assert_rename!(&tmp, "fstab.to-edit", Event::from(etc_token), "fstab.updated");

            open_as_vmo_file_assert_content!(&proxy, ro_flags, "etc/fstab.updated", "/dev/fs /");
            open_as_file_assert_err!(&proxy, ro_flags, "tmp/fstab.to-edit", Status::NOT_FOUND);

            assert_close!(etc);
            assert_close!(tmp);
            assert_close!(proxy);
        },
    )
    .run();
}

/// This test should be exactly the same as the [`rename_within_directory`], but with watcher
/// messages monitoring.  It should help narrow down the issue when something fails, by immediately
/// showing if it is watchers related or not.
#[test]
fn rename_within_directory_with_watchers() {
    let root = mut_pseudo_directory! {
        "passwd" => read_only_static(b"/dev/fs /"),
    };

    test_server_client(
        fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_WRITABLE,
        root,
        |proxy| async move {
            let watcher_client = {
                let mask =
                    fio::WatchMask::EXISTING | fio::WatchMask::ADDED | fio::WatchMask::REMOVED;

                let watcher_client = assert_watch!(proxy, mask);

                assert_watcher_one_message_watched_events!(
                    watcher_client,
                    { EXISTING, "." },
                    { EXISTING, "passwd" },
                );
                watcher_client
            };

            let ro_flags = fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::DESCRIBE;

            open_as_vmo_file_assert_content!(&proxy, ro_flags, "passwd", "/dev/fs /");

            let mut root_token = assert_get_token!(&proxy);
            // This should return an error because the source file does not exist
            assert_rename_err!(
                &proxy,
                "file-does-not-exist",
                Event::from(root_token),
                "file-will-not-exist",
                Status::NOT_FOUND
            );

            root_token = assert_get_token!(&proxy);
            assert_rename!(&proxy, "passwd", Event::from(root_token), "fstab");

            // If the unsuccessful rename produced events, they will be read first
            // instead of the expected events for the successful rename.
            assert_watcher_one_message_watched_events!(watcher_client, { REMOVED, "passwd" });
            assert_watcher_one_message_watched_events!(watcher_client, { ADDED, "fstab" });

            open_as_file_assert_err!(&proxy, ro_flags, "passwd", Status::NOT_FOUND);
            open_as_vmo_file_assert_content!(&proxy, ro_flags, "fstab", "/dev/fs /");

            drop(watcher_client);
            assert_close!(proxy);
        },
    )
    .run();
}

/// This test should be exactly the same as the [`rename_across_directories`], but with watcher
/// messages monitoring.  It should help narrow down the issue when something fails, by immediately
/// showing if it is watchers related or not.
#[test]
fn rename_across_directories_with_watchers() {
    let root = mut_pseudo_directory! {
        "tmp" => mut_pseudo_directory! {
            "fstab.new" => read_only_static(b"/dev/fs /"),
        },
        "etc" => mut_pseudo_directory! {},
    };

    test_server_client(
        fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_WRITABLE,
        root,
        |proxy| async move {
            let ro_flags = fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::DESCRIBE;
            let rw_flags = fio::OpenFlags::RIGHT_READABLE
                | fio::OpenFlags::RIGHT_WRITABLE
                | fio::OpenFlags::DESCRIBE;

            open_as_vmo_file_assert_content!(&proxy, ro_flags, "tmp/fstab.new", "/dev/fs /");
            open_as_file_assert_err!(&proxy, ro_flags, "etc/fstab", Status::NOT_FOUND);

            let tmp = open_get_directory_proxy_assert_ok!(&proxy, rw_flags, "tmp");
            let etc = open_get_directory_proxy_assert_ok!(&proxy, rw_flags, "etc");

            let etc_token = assert_get_token!(&etc);

            let watchers_mask =
                fio::WatchMask::EXISTING | fio::WatchMask::ADDED | fio::WatchMask::REMOVED;

            let tmp_watcher = {
                let watcher = assert_watch!(tmp, watchers_mask);

                assert_watcher_one_message_watched_events!(
                    watcher,
                    { EXISTING, "." },
                    { EXISTING, "fstab.new" },
                );
                watcher
            };

            let etc_watcher = {
                let watcher = assert_watch!(etc, watchers_mask);

                assert_watcher_one_message_watched_events!(watcher, { EXISTING, "." });
                watcher
            };

            assert_rename!(&tmp, "fstab.new", Event::from(etc_token), "fstab");

            assert_watcher_one_message_watched_events!(tmp_watcher, { REMOVED, "fstab.new" });
            assert_watcher_one_message_watched_events!(etc_watcher, { ADDED, "fstab" });

            open_as_file_assert_err!(&proxy, ro_flags, "tmp/fstab.new", Status::NOT_FOUND);
            open_as_vmo_file_assert_content!(&proxy, ro_flags, "etc/fstab", "/dev/fs /");

            drop(tmp_watcher);
            drop(etc_watcher);
            assert_close!(tmp);
            assert_close!(etc);
            assert_close!(proxy);
        },
    )
    .run();
}

/// This test should be exactly the same as the [`rename_across_directories_twice`], but with
/// watcher messages monitoring.  It should help narrow down the issue when something fails, by
/// immediately showing if it is watchers related or not.
#[test]
fn rename_across_directories_twice_with_watchers() {
    let root = mut_pseudo_directory! {
        "etc" => mut_pseudo_directory! {
            "fstab" => read_only_static(b"/dev/fs /"),
        },
        "tmp" => mut_pseudo_directory! {},
    };

    test_server_client(
        fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_WRITABLE,
        root,
        |proxy| async move {
            let ro_flags = fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::DESCRIBE;
            let rw_flags = fio::OpenFlags::RIGHT_READABLE
                | fio::OpenFlags::RIGHT_WRITABLE
                | fio::OpenFlags::DESCRIBE;

            open_as_vmo_file_assert_content!(&proxy, ro_flags, "etc/fstab", "/dev/fs /");
            open_as_file_assert_err!(&proxy, ro_flags, "tmp/fstab.to-edit", Status::NOT_FOUND);

            let etc = open_get_directory_proxy_assert_ok!(&proxy, rw_flags, "etc");
            let tmp = open_get_directory_proxy_assert_ok!(&proxy, rw_flags, "tmp");

            let etc_token = assert_get_token!(&etc);
            let tmp_token = assert_get_token!(&tmp);

            let watchers_mask =
                fio::WatchMask::EXISTING | fio::WatchMask::ADDED | fio::WatchMask::REMOVED;

            let etc_watcher = {
                let watcher = assert_watch!(etc, watchers_mask);

                assert_watcher_one_message_watched_events!(
                    watcher,
                    { EXISTING, "." },
                    { EXISTING, "fstab" },
                );
                watcher
            };

            let tmp_watcher = {
                let watcher = assert_watch!(tmp, watchers_mask);

                assert_watcher_one_message_watched_events!(watcher, { EXISTING, "." });
                watcher
            };

            assert_rename!(&etc, "fstab", Event::from(tmp_token), "fstab.to-edit");

            assert_watcher_one_message_watched_events!(etc_watcher, { REMOVED, "fstab" });
            assert_watcher_one_message_watched_events!(tmp_watcher, { ADDED, "fstab.to-edit" });

            open_as_file_assert_err!(&proxy, ro_flags, "etc/fstab", Status::NOT_FOUND);
            open_as_vmo_file_assert_content!(&proxy, ro_flags, "tmp/fstab.to-edit", "/dev/fs /");

            assert_rename!(&tmp, "fstab.to-edit", Event::from(etc_token), "fstab.updated");

            assert_watcher_one_message_watched_events!(tmp_watcher, { REMOVED, "fstab.to-edit" });
            assert_watcher_one_message_watched_events!(etc_watcher, { ADDED, "fstab.updated" });

            open_as_vmo_file_assert_content!(&proxy, ro_flags, "etc/fstab.updated", "/dev/fs /");
            open_as_file_assert_err!(&proxy, ro_flags, "tmp/fstab.to-edit", Status::NOT_FOUND);

            drop(etc_watcher);
            drop(tmp_watcher);
            assert_close!(etc);
            assert_close!(tmp);
            assert_close!(proxy);
        },
    )
    .run();
}

#[test]
fn rename_into_self_with_watchers() {
    let root = mut_pseudo_directory! {
        "passwd" => read_only_static(b"[redacted]"),
    };

    test_server_client(
        fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_WRITABLE,
        root,
        |proxy| async move {
            let watcher_client = {
                let mask =
                    fio::WatchMask::EXISTING | fio::WatchMask::ADDED | fio::WatchMask::REMOVED;

                let watcher_client = assert_watch!(proxy, mask);

                assert_watcher_one_message_watched_events!(
                    watcher_client,
                    { EXISTING, "." },
                    { EXISTING, "passwd" },
                );
                watcher_client
            };

            let ro_flags = fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::DESCRIBE;

            open_as_vmo_file_assert_content!(&proxy, ro_flags, "passwd", "[redacted]");

            let root_token = assert_get_token!(&proxy);
            assert_rename!(&proxy, "passwd", Event::from(root_token), "passwd");

            assert_watcher_one_message_watched_events!(watcher_client, { REMOVED, "passwd" });
            assert_watcher_one_message_watched_events!(watcher_client, { ADDED, "passwd" });

            open_as_vmo_file_assert_content!(&proxy, ro_flags, "passwd", "[redacted]");

            drop(watcher_client);
            assert_close!(proxy);
        },
    )
    .run();
}

#[test]
fn get_token_fails_for_read_only_target() {
    let root = mut_pseudo_directory! {
        "etc" => mut_pseudo_directory! {
            "passwd" => read_only_static(b"[redacted]"),
        },
    };

    test_server_client(
        fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_WRITABLE,
        root,
        |proxy| async move {
            let ro_flags = fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::DESCRIBE;

            let etc = open_get_directory_proxy_assert_ok!(&proxy, ro_flags, "etc");
            assert_get_token_err!(&etc, Status::BAD_HANDLE);

            assert_close!(etc);
            assert_close!(proxy);
        },
    )
    .run();
}

#[test]
fn rename_fails_for_read_only_source() {
    let root = mut_pseudo_directory! {
        "etc" => mut_pseudo_directory! {
            "fstab" => read_only_static(b"/dev/fs /"),
        },
        "tmp" => mut_pseudo_directory! {},
    };

    test_server_client(
        fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_WRITABLE,
        root,
        |proxy| async move {
            let ro_flags = fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::DESCRIBE;
            let rw_flags = fio::OpenFlags::RIGHT_READABLE
                | fio::OpenFlags::RIGHT_WRITABLE
                | fio::OpenFlags::DESCRIBE;

            let etc = open_get_directory_proxy_assert_ok!(&proxy, ro_flags, "etc");

            let tmp = open_get_directory_proxy_assert_ok!(&proxy, rw_flags, "tmp");
            let tmp_token = assert_get_token!(&tmp);

            assert_rename_err!(&etc, "fstab", Event::from(tmp_token), "fstab", Status::BAD_HANDLE);

            assert_close!(etc);
            assert_close!(tmp);
            assert_close!(proxy);
        },
    )
    .run();
}

#[test]
fn hardlink_not_supported() {
    let root = mut_pseudo_directory! {
        "test" => read_only_static(b"hello"),
        "tmp" => mut_pseudo_directory! {},
    };

    test_server_client(
        fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_WRITABLE,
        root,
        |proxy| async move {
            let flags = fio::OpenFlags::RIGHT_READABLE
                | fio::OpenFlags::RIGHT_WRITABLE
                | fio::OpenFlags::DESCRIBE;

            let tmp = open_get_directory_proxy_assert_ok!(&proxy, flags, "tmp");
            let tmp_token = assert_get_token!(&tmp);

            assert_link_err!(&proxy, "test", tmp_token, "linked-test", Status::NOT_SUPPORTED);

            assert_close!(tmp);
            assert_close!(proxy);
        },
    )
    .run();
}

#[test]
fn create_file() {
    let count = Arc::new(AtomicU8::new(0));

    let constructor = tree_constructor(move |_parent, name| {
        let index = count.fetch_add(1, Ordering::Relaxed);
        let content = format!("{} - {}", name, index).into_bytes();
        Ok(read_only_static(content.clone()))
    });

    let root = mut_pseudo_directory! {
        "etc" => mut_pseudo_directory! {},
        "tmp" => mut_pseudo_directory! {},
    };

    test_server_client(
        fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_WRITABLE,
        root,
        |proxy| async move {
            let flags = fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::DESCRIBE;
            let create_flags = flags | fio::OpenFlags::CREATE;

            open_as_vmo_file_assert_content!(&proxy, create_flags, "etc/fstab", "fstab - 0");

            let etc = open_get_directory_proxy_assert_ok!(&proxy, flags, "etc");

            {
                let mut expected = DirentsSameInodeBuilder::new(fio::INO_UNKNOWN);
                expected.add(fio::DirentType::Directory, b".").add(fio::DirentType::File, b"fstab");

                assert_read_dirents!(etc, 1000, expected.into_vec());
            }

            assert_close!(proxy);
        },
    )
    .entry_constructor(constructor)
    .run();
}

#[test]
fn create_directory() {
    let constructor = tree_constructor(|_parent, _name| panic!("No files should be created"));

    let root = mut_pseudo_directory! {
        "tmp" => mut_pseudo_directory! {},
    };

    test_server_client(
        fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_WRITABLE,
        root,
        |proxy| async move {
            let flags = fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::DESCRIBE;
            let create_directory_flags = flags | fio::OpenFlags::DIRECTORY | fio::OpenFlags::CREATE;

            let etc = open_get_directory_proxy_assert_ok!(&proxy, create_directory_flags, "etc");

            {
                let mut expected = DirentsSameInodeBuilder::new(fio::INO_UNKNOWN);
                expected
                    .add(fio::DirentType::Directory, b".")
                    .add(fio::DirentType::Directory, b"etc")
                    .add(fio::DirentType::Directory, b"tmp");

                assert_read_dirents!(proxy, 1000, expected.into_vec());
            }

            assert_close!(etc);
            assert_close!(proxy);
        },
    )
    .entry_constructor(constructor)
    .run();
}

#[test]
fn create_two_levels_deep() {
    let count = Arc::new(AtomicU8::new(0));

    let constructor = tree_constructor(move |_parent, name| {
        let index = count.fetch_add(1, Ordering::Relaxed);
        let content = format!("{} - {}", name, index).into_bytes();
        Ok(read_only_static(content.clone()))
    });

    let root = mut_pseudo_directory! {
        "tmp" => mut_pseudo_directory! {},
    };

    test_server_client(
        fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_WRITABLE,
        root,
        |proxy| async move {
            let flags = fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::DESCRIBE;
            let create_directory_flags = flags | fio::OpenFlags::DIRECTORY | fio::OpenFlags::CREATE;
            let create_flags = flags | fio::OpenFlags::CREATE;

            let etc = open_get_directory_proxy_assert_ok!(&proxy, create_directory_flags, "etc");

            {
                let mut expected = DirentsSameInodeBuilder::new(fio::INO_UNKNOWN);
                expected
                    .add(fio::DirentType::Directory, b".")
                    .add(fio::DirentType::Directory, b"etc")
                    .add(fio::DirentType::Directory, b"tmp");

                assert_read_dirents!(proxy, 1000, expected.into_vec());
            }

            open_as_vmo_file_assert_content!(&proxy, create_flags, "etc/fstab", "fstab - 0");
            open_as_vmo_file_assert_content!(&proxy, create_flags, "etc/passwd", "passwd - 1");

            {
                let mut expected = DirentsSameInodeBuilder::new(fio::INO_UNKNOWN);
                expected
                    .add(fio::DirentType::Directory, b".")
                    .add(fio::DirentType::File, b"fstab")
                    .add(fio::DirentType::File, b"passwd");

                assert_read_dirents!(etc, 1000, expected.into_vec());
            }

            assert_close!(etc);
            assert_close!(proxy);
        },
    )
    .entry_constructor(constructor)
    .run();
}

#[test]
fn can_not_create_nested() {
    // This tree constructor does not allow creation of non-leaf entries. Only the very last path
    // component might be created.
    let constructor = tree_constructor(|_parent, _name| panic!("No files should be created"));

    let root = mut_pseudo_directory! {};

    test_server_client(
        fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::RIGHT_WRITABLE,
        root,
        |proxy| async move {
            let flags = fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::DESCRIBE;
            let create_flags = flags | fio::OpenFlags::CREATE;

            open_as_file_assert_err!(&proxy, create_flags, "etc/fstab", Status::NOT_FOUND);
            open_as_directory_assert_err!(&proxy, create_flags, "tmp/log", Status::NOT_FOUND);

            {
                let mut expected = DirentsSameInodeBuilder::new(fio::INO_UNKNOWN);
                expected.add(fio::DirentType::Directory, b".");

                assert_read_dirents!(proxy, 1000, expected.into_vec());
            }

            assert_close!(proxy);
        },
    )
    .entry_constructor(constructor)
    .run();
}
