// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Tests for the lazy directory.

use super::{lazy, lazy_with_watchers, LazyDirectory, WatcherEvent};

// Macros are exported into the root of the crate.
use crate::{
    assert_channel_closed, assert_close, assert_event, assert_read, assert_read_dirents,
    assert_read_dirents_err, open_get_directory_proxy_assert_ok, open_get_proxy_assert,
    open_get_vmo_file_proxy_assert_ok,
};

use crate::{
    directory::{
        dirents_sink::{self, AppendResult},
        entry::{DirectoryEntry, EntryInfo},
        test_utils::{run_server_client, test_server_client, DirentsSameInodeBuilder},
        traversal_position::TraversalPosition::{self, End, Name, Start},
    },
    execution_scope::ExecutionScope,
    file::vmo::asynchronous::{read_only_const, read_only_static},
};

use {
    async_trait::async_trait,
    fidl_fuchsia_io as fio,
    fuchsia_async::TestExecutor,
    fuchsia_zircon::Status,
    futures::{channel::mpsc, lock::Mutex},
    std::{
        marker::{Send, Sync},
        sync::{
            atomic::{AtomicU8, Ordering},
            Arc,
        },
    },
    vfs_macros::pseudo_directory,
};

struct Entries {
    entries: Box<[(TraversalPosition, EntryInfo)]>,
    get_entry_fn:
        Box<dyn Fn(&str) -> Result<Arc<dyn DirectoryEntry>, Status> + Send + Sync + 'static>,
}

fn not_found(_name: &str) -> Result<Arc<dyn DirectoryEntry>, Status> {
    Err(Status::NOT_FOUND)
}

const DOT: (fio::DirentType, &'static str) = (fio::DirentType::Directory, ".");

impl Entries {
    fn new<F: Fn(&str) -> Result<Arc<dyn DirectoryEntry>, Status> + Send + Sync + 'static>(
        mut entries: Vec<(fio::DirentType, &'static str)>,
        get_entry_fn: F,
    ) -> Self {
        entries.sort_unstable_by_key(|&(_, name)| name);
        let entries = entries
            .into_iter()
            .map(|(dirent_type, name)| {
                let pos = if name == "." {
                    TraversalPosition::Start
                } else {
                    TraversalPosition::Name(name.to_string())
                };
                (pos, EntryInfo::new(fio::INO_UNKNOWN, dirent_type))
            })
            .collect::<Box<[(TraversalPosition, EntryInfo)]>>();
        Entries { entries, get_entry_fn: Box::new(get_entry_fn) }
    }
}

#[async_trait]
impl LazyDirectory for Entries {
    async fn read_dirents<'a>(
        &'a self,
        pos: &'a TraversalPosition,
        mut sink: Box<dyn dirents_sink::Sink>,
    ) -> Result<(TraversalPosition, Box<dyn dirents_sink::Sealed>), Status> {
        let candidate = self.entries.binary_search_by(|(entry_pos, _)| entry_pos.cmp(pos));
        let mut i = match candidate {
            Ok(i) => i,
            Err(i) => i,
        };

        while i < self.entries.len() {
            let (entry_pos, entry_info) = &self.entries[i];
            let name = match entry_pos {
                Start => ".",
                Name(name) => name,
                End => panic!("`entries` does not contain End"),
                _ => unreachable!(),
            };

            sink = match sink.append(&entry_info, name) {
                AppendResult::Ok(sink) => sink,
                AppendResult::Sealed(done) => {
                    return Ok((entry_pos.clone(), done));
                }
            };

            i += 1;
        }

        Ok((TraversalPosition::End, sink.seal()))
    }

    async fn get_entry(&self, name: &str) -> Result<Arc<dyn DirectoryEntry>, Status> {
        (self.get_entry_fn)(name)
    }
}

#[test]
fn empty() {
    run_server_client(
        fio::OpenFlags::RIGHT_READABLE,
        lazy(Entries::new(vec![], not_found)),
        |root| async move {
            assert_close!(root);
        },
    );
}

#[test]
fn empty_with_watchers() {
    let (mut watcher_events, watcher_events_consumer) = mpsc::unbounded();

    let exec = TestExecutor::new();
    let scope = ExecutionScope::new();

    let server =
        lazy_with_watchers(scope.clone(), Entries::new(vec![], not_found), watcher_events_consumer);

    test_server_client(fio::OpenFlags::RIGHT_READABLE, server, |root| async move {
        assert_close!(root);
        watcher_events.disconnect();
    })
    .exec(exec)
    .run();
}

#[test]
fn static_listing() {
    let entries = vec![
        DOT,
        (fio::DirentType::File, "one"),
        (fio::DirentType::File, "two"),
        (fio::DirentType::File, "three"),
    ];

    run_server_client(
        fio::OpenFlags::RIGHT_READABLE,
        lazy(Entries::new(entries, not_found)),
        |root| {
            async move {
                {
                    let mut expected = DirentsSameInodeBuilder::new(fio::INO_UNKNOWN);
                    // Note that the build_sorted_static_get_entry_names() will sort entries
                    // alphabetically when returning them, so we see a different order here.
                    expected
                        .add(fio::DirentType::Directory, b".")
                        .add(fio::DirentType::File, b"one")
                        .add(fio::DirentType::File, b"three")
                        .add(fio::DirentType::File, b"two");

                    assert_read_dirents!(root, 1000, expected.into_vec());
                }

                assert_close!(root);
            }
        },
    );
}

#[test]
fn static_entries() {
    let entries = vec![
        DOT,
        (fio::DirentType::File, "one"),
        (fio::DirentType::File, "two"),
        (fio::DirentType::File, "three"),
    ];

    let get_entry = |name: &str| {
        let content = format!("File {} content", name);
        let bytes = content.into_bytes();
        Ok(read_only_static(bytes) as Arc<dyn DirectoryEntry>)
    };

    run_server_client(
        fio::OpenFlags::RIGHT_READABLE,
        lazy(Entries::new(entries, get_entry)),
        |root| async move {
            let flags = fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::DESCRIBE;
            open_as_vmo_file_assert_content!(&root, flags, "one", "File one content");
            open_as_vmo_file_assert_content!(&root, flags, "two", "File two content");
            open_as_vmo_file_assert_content!(&root, flags, "three", "File three content");

            assert_close!(root);
        },
    );
}

#[test]
fn static_entries_with_traversal() {
    let entries = vec![DOT, (fio::DirentType::Directory, "etc"), (fio::DirentType::File, "files")];

    let get_entry = |name: &str| match name {
        "etc" => {
            let etc = pseudo_directory! {
                "fstab" => read_only_static(b"/dev/fs /"),
                "ssh" => pseudo_directory! {
                    "sshd_config" => read_only_static(b"# Empty"),
                },
            };
            Ok(etc as Arc<dyn DirectoryEntry>)
        }
        "files" => Ok(read_only_static(b"Content") as Arc<dyn DirectoryEntry>),
        _ => Err(Status::NOT_FOUND),
    };

    run_server_client(
        fio::OpenFlags::RIGHT_READABLE,
        lazy(Entries::new(entries, get_entry)),
        |root| async move {
            let flags = fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::DESCRIBE;
            {
                let mut expected = DirentsSameInodeBuilder::new(fio::INO_UNKNOWN);
                expected
                    .add(fio::DirentType::Directory, b".")
                    .add(fio::DirentType::Directory, b"etc")
                    .add(fio::DirentType::File, b"files");

                assert_read_dirents!(root, 1000, expected.into_vec());
            }

            {
                let etc_dir = open_get_directory_proxy_assert_ok!(&root, flags, "etc");

                let mut expected = DirentsSameInodeBuilder::new(fio::INO_UNKNOWN);
                expected
                    .add(fio::DirentType::Directory, b".")
                    .add(fio::DirentType::File, b"fstab")
                    .add(fio::DirentType::Directory, b"ssh");

                assert_read_dirents!(etc_dir, 1000, expected.into_vec());
                assert_close!(etc_dir);
            }

            {
                let ssh_dir = open_get_directory_proxy_assert_ok!(&root, flags, "etc/ssh");

                let mut expected = DirentsSameInodeBuilder::new(fio::INO_UNKNOWN);
                expected
                    .add(fio::DirentType::Directory, b".")
                    .add(fio::DirentType::File, b"sshd_config");

                assert_read_dirents!(ssh_dir, 1000, expected.into_vec());
                assert_close!(ssh_dir);
            }

            open_as_vmo_file_assert_content!(&root, flags, "etc/fstab", "/dev/fs /");
            open_as_vmo_file_assert_content!(&root, flags, "files", "Content");

            assert_close!(root);
        },
    );
}

// DynamicEntries is a helper that will return a different set of entries for each iteration.
struct DynamicEntries {
    entries: Box<[Entries]>,
    index: Mutex<usize>,
}

impl DynamicEntries {
    fn new(entries: Box<[Entries]>) -> Self {
        DynamicEntries { entries, index: Mutex::new(0) }
    }
}

#[async_trait]
impl LazyDirectory for DynamicEntries {
    async fn read_dirents<'a>(
        &'a self,
        pos: &'a TraversalPosition,
        sink: Box<dyn dirents_sink::Sink>,
    ) -> Result<(TraversalPosition, Box<dyn dirents_sink::Sealed>), Status> {
        let mut index = self.index.lock().await;
        let result = self.entries[*index].read_dirents(pos, sink).await;
        // If we finished an iteration, move on to the next set of entries.
        if let Ok((TraversalPosition::End, _)) = result {
            if *index + 1 < self.entries.len() {
                *index += 1;
            }
        }
        result
    }

    async fn get_entry(&self, _name: &str) -> Result<Arc<dyn DirectoryEntry>, Status> {
        Err(Status::NOT_FOUND)
    }
}

#[test]
fn dynamic_listing() {
    let listing1 = vec![DOT, (fio::DirentType::File, "one"), (fio::DirentType::File, "two")];
    let listing2 = vec![DOT, (fio::DirentType::File, "two"), (fio::DirentType::File, "three")];

    let entries = DynamicEntries::new(
        vec![Entries::new(listing1, not_found), Entries::new(listing2, not_found)].into(),
    );

    run_server_client(fio::OpenFlags::RIGHT_READABLE, lazy(entries), |root| {
        async move {
            {
                let mut expected = DirentsSameInodeBuilder::new(fio::INO_UNKNOWN);
                // Note that the build_sorted_static_get_entry_names() will sort entries
                // alphabetically when returning them, so we see a different order here.
                expected
                    .add(fio::DirentType::Directory, b".")
                    .add(fio::DirentType::File, b"one")
                    .add(fio::DirentType::File, b"two");

                assert_read_dirents!(root, 1000, expected.into_vec());
            }

            assert_rewind!(root);

            {
                let mut expected = DirentsSameInodeBuilder::new(fio::INO_UNKNOWN);
                // Note that the build_sorted_static_get_entry_names() will sort entries
                // alphabetically when returning them, so we see a different order here.
                expected
                    .add(fio::DirentType::Directory, b".")
                    .add(fio::DirentType::File, b"three")
                    .add(fio::DirentType::File, b"two");

                assert_read_dirents!(root, 1000, expected.into_vec());
            }

            assert_close!(root);
        }
    });
}

#[test]
fn dynamic_entries() {
    let entries = vec![DOT, (fio::DirentType::File, "file1"), (fio::DirentType::File, "file2")];

    let count = Arc::new(AtomicU8::new(0));
    let get_entry = move |name: &str| {
        let entry = |count: u8| {
            let content = format!("Content: {}", count);

            Ok(read_only_const(content.as_bytes()) as Arc<dyn DirectoryEntry>)
        };

        match name {
            "file1" => {
                let count = count.fetch_add(1, Ordering::Relaxed) + 1;
                entry(count)
            }
            "file2" => {
                let count = count.fetch_add(10, Ordering::Relaxed) + 10;
                entry(count)
            }
            _ => Err(Status::NOT_FOUND),
        }
    };

    run_server_client(
        fio::OpenFlags::RIGHT_READABLE,
        lazy(Entries::new(entries, get_entry)),
        |root| async move {
            let flags = fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::DESCRIBE;

            open_as_vmo_file_assert_content!(&root, flags, "file1", "Content: 1");
            open_as_vmo_file_assert_content!(&root, flags, "file1", "Content: 2");
            open_as_vmo_file_assert_content!(&root, flags, "file2", "Content: 12");
            open_as_vmo_file_assert_content!(&root, flags, "file2", "Content: 22");
            open_as_vmo_file_assert_content!(&root, flags, "file1", "Content: 23");

            assert_close!(root);
        },
    );
}

#[test]
fn read_dirents_small_buffer() {
    let entries = vec![
        DOT,
        (fio::DirentType::Directory, "etc"),
        (fio::DirentType::File, "files"),
        (fio::DirentType::File, "more"),
        (fio::DirentType::File, "uname"),
    ];

    run_server_client(
        fio::OpenFlags::RIGHT_READABLE,
        lazy(Entries::new(entries, not_found)),
        |root| {
            async move {
                {
                    let mut expected = DirentsSameInodeBuilder::new(fio::INO_UNKNOWN);
                    // Entry header is 10 bytes + length of the name in bytes.
                    // (10 + 1) = 11
                    expected.add(fio::DirentType::Directory, b".");
                    assert_read_dirents!(root, 11, expected.into_vec());
                }

                {
                    let mut expected = DirentsSameInodeBuilder::new(fio::INO_UNKNOWN);
                    expected
                        // (10 + 3) = 13
                        .add(fio::DirentType::Directory, b"etc")
                        // 13 + (10 + 5) = 28
                        .add(fio::DirentType::File, b"files");
                    assert_read_dirents!(root, 28, expected.into_vec());
                }

                {
                    let mut expected = DirentsSameInodeBuilder::new(fio::INO_UNKNOWN);
                    expected
                        .add(fio::DirentType::File, b"more")
                        .add(fio::DirentType::File, b"uname");
                    assert_read_dirents!(root, 100, expected.into_vec());
                }

                assert_read_dirents!(root, 100, vec![]);

                assert_close!(root);
            }
        },
    );
}

#[test]
fn read_dirents_very_small_buffer() {
    let entries = vec![DOT, (fio::DirentType::File, "file")];

    run_server_client(
        fio::OpenFlags::RIGHT_READABLE,
        lazy(Entries::new(entries, not_found)),
        |root| {
            async move {
                // Entry header is 10 bytes, so this read should not be able to return a single
                // entry.
                assert_read_dirents_err!(root, 8, Status::BUFFER_TOO_SMALL);

                {
                    let mut expected = DirentsSameInodeBuilder::new(fio::INO_UNKNOWN);
                    expected
                        .add(fio::DirentType::Directory, b".")
                        .add(fio::DirentType::File, b"file");
                    assert_read_dirents!(root, 100, expected.into_vec());
                }

                assert_close!(root);
            }
        },
    );
}

#[test]
fn watch_empty() {
    let (_watcher_sender, watcher_stream) = mpsc::unbounded::<WatcherEvent>();

    let exec = TestExecutor::new();
    let scope = ExecutionScope::new();

    let root = lazy_with_watchers(scope.clone(), Entries::new(vec![], not_found), watcher_stream);
    test_server_client(fio::OpenFlags::RIGHT_READABLE, root, |root| async move {
        let mask = fio::WatchMask::EXISTING
            | fio::WatchMask::IDLE
            | fio::WatchMask::ADDED
            | fio::WatchMask::REMOVED;
        let watcher_client = assert_watch!(root, mask);

        assert_watcher_one_message_watched_events!(watcher_client, { IDLE, vec![] });

        drop(watcher_client);
        assert_close!(root);
    })
    .exec(exec)
    .run();
}

#[test]
fn watch_non_empty() {
    let entries = vec![
        DOT,
        (fio::DirentType::File, "one"),
        (fio::DirentType::File, "two"),
        (fio::DirentType::File, "three"),
    ];
    let (_watcher_sender, watcher_stream) = mpsc::unbounded::<WatcherEvent>();

    let exec = TestExecutor::new();
    let scope = ExecutionScope::new();

    let root = lazy_with_watchers(scope.clone(), Entries::new(entries, not_found), watcher_stream);

    test_server_client(fio::OpenFlags::RIGHT_READABLE, root, |root| async move {
        let mask = fio::WatchMask::EXISTING
            | fio::WatchMask::IDLE
            | fio::WatchMask::ADDED
            | fio::WatchMask::REMOVED;
        let watcher_client = assert_watch!(root, mask);

        assert_watcher_one_message_watched_events!(
            watcher_client,
            { EXISTING, "." },
            { EXISTING, "one" },
            { EXISTING, "three" },
            { EXISTING, "two" },
        );
        assert_watcher_one_message_watched_events!(watcher_client, { IDLE, vec![] });

        drop(watcher_client);
        assert_close!(root);
    })
    .exec(exec)
    .run();
}

#[test]
fn watch_two_watchers() {
    let entries = vec![
        DOT,
        (fio::DirentType::File, "one"),
        (fio::DirentType::File, "two"),
        (fio::DirentType::File, "three"),
    ];
    let (_watcher_sender, watcher_stream) = mpsc::unbounded::<WatcherEvent>();

    let exec = TestExecutor::new();
    let scope = ExecutionScope::new();

    let root = lazy_with_watchers(scope.clone(), Entries::new(entries, not_found), watcher_stream);

    test_server_client(fio::OpenFlags::RIGHT_READABLE, root, |root| async move {
        let mask = fio::WatchMask::EXISTING
            | fio::WatchMask::IDLE
            | fio::WatchMask::ADDED
            | fio::WatchMask::REMOVED;
        let watcher1_client = assert_watch!(root, mask);

        assert_watcher_one_message_watched_events!(
            watcher1_client,
            { EXISTING, "." },
            { EXISTING, "one" },
            { EXISTING, "three" },
            { EXISTING, "two" },
        );
        assert_watcher_one_message_watched_events!(watcher1_client, { IDLE, vec![] });

        let watcher2_client = assert_watch!(root, mask);

        assert_watcher_one_message_watched_events!(
            watcher2_client,
            { EXISTING, "." },
            { EXISTING, "one" },
            { EXISTING, "three" },
            { EXISTING, "two" },
        );
        assert_watcher_one_message_watched_events!(watcher2_client, { IDLE, vec![] });

        drop(watcher1_client);
        drop(watcher2_client);
        assert_close!(root);
    })
    .exec(exec)
    .run();
}

#[test]
fn watch_with_mask() {
    let entries = vec![
        DOT,
        (fio::DirentType::File, "one"),
        (fio::DirentType::File, "two"),
        (fio::DirentType::File, "three"),
    ];
    let (_watcher_sender, watcher_stream) = mpsc::unbounded::<WatcherEvent>();

    let exec = TestExecutor::new();
    let scope = ExecutionScope::new();

    let root = lazy_with_watchers(scope.clone(), Entries::new(entries, not_found), watcher_stream);

    test_server_client(fio::OpenFlags::RIGHT_READABLE, root, |root| async move {
        let mask = fio::WatchMask::IDLE | fio::WatchMask::ADDED | fio::WatchMask::REMOVED;
        let watcher_client = assert_watch!(root, mask);

        assert_watcher_one_message_watched_events!(watcher_client, { IDLE, vec![] });

        drop(watcher_client);
        assert_close!(root);
    })
    .exec(exec)
    .run();
}

#[test]
fn watch_addition() {
    let entries = vec![DOT, (fio::DirentType::File, "one")];

    let (watcher_sender, watcher_stream) = mpsc::unbounded::<WatcherEvent>();

    let exec = TestExecutor::new();
    let scope = ExecutionScope::new();

    let root = lazy_with_watchers(scope.clone(), Entries::new(entries, not_found), watcher_stream);

    test_server_client(fio::OpenFlags::RIGHT_READABLE, root, |root| async move {
        let mask = fio::WatchMask::ADDED | fio::WatchMask::REMOVED;
        let watcher_client = assert_watch!(root, mask);

        watcher_sender
            .unbounded_send(WatcherEvent::Added(vec!["two".to_string()]))
            .expect("watcher_sender.send() failed");

        assert_watcher_one_message_watched_events!(watcher_client, { ADDED, "two" });

        watcher_sender
            .unbounded_send(WatcherEvent::Added(vec!["three".to_string(), "four".to_string()]))
            .expect("watcher_sender.send() failed");

        assert_watcher_one_message_watched_events!(
            watcher_client,
            { ADDED, "three" },
            { ADDED, "four" },
        );

        assert_close!(root);
    })
    .exec(exec)
    .run();
}

#[test]
fn watch_removal() {
    let entries = vec![
        DOT,
        (fio::DirentType::File, "one"),
        (fio::DirentType::File, "two"),
        (fio::DirentType::File, "three"),
        (fio::DirentType::File, "four"),
    ];

    let (watcher_sender, watcher_stream) = mpsc::unbounded::<WatcherEvent>();

    let exec = TestExecutor::new();
    let scope = ExecutionScope::new();

    let root = lazy_with_watchers(scope.clone(), Entries::new(entries, not_found), watcher_stream);

    test_server_client(fio::OpenFlags::RIGHT_READABLE, root, |root| async move {
        let mask = fio::WatchMask::ADDED | fio::WatchMask::REMOVED;
        let watcher_client = assert_watch!(root, mask);

        watcher_sender
            .unbounded_send(WatcherEvent::Removed(vec!["two".to_string()]))
            .expect("watcher_sender.send() failed");

        assert_watcher_one_message_watched_events!(watcher_client, { REMOVED, "two" });

        watcher_sender
            .unbounded_send(WatcherEvent::Removed(vec!["three".to_string(), "four".to_string()]))
            .expect("watcher_sender.send() failed");

        assert_watcher_one_message_watched_events!(
            watcher_client,
            { REMOVED, "three" },
            { REMOVED, "four" },
        );

        assert_close!(root);
    })
    .exec(exec)
    .run();
}

#[test]
fn watch_watcher_stream_closed() {
    let entries = vec![
        DOT,
        (fio::DirentType::File, "one"),
        (fio::DirentType::File, "two"),
        (fio::DirentType::File, "three"),
    ];
    // Dropping the sender will close the receiver end.
    let (_, watcher_stream) = mpsc::unbounded::<WatcherEvent>();

    let exec = TestExecutor::new();
    let scope = ExecutionScope::new();

    let root = lazy_with_watchers(scope.clone(), Entries::new(entries, not_found), watcher_stream);

    test_server_client(fio::OpenFlags::RIGHT_READABLE, root, |root| async move {
        let mask = fio::WatchMask::EXISTING | fio::WatchMask::IDLE;
        assert_watch_err!(root, mask, Status::NOT_SUPPORTED);

        assert_close!(root);
    })
    .exec(exec)
    .run();
}

#[test]
fn watch_close_watcher_stream() {
    let entries = vec![
        DOT,
        (fio::DirentType::File, "one"),
        (fio::DirentType::File, "two"),
        (fio::DirentType::File, "three"),
    ];
    let (watcher_sender, watcher_stream) = mpsc::unbounded::<WatcherEvent>();

    let exec = TestExecutor::new();
    let scope = ExecutionScope::new();

    let root = lazy_with_watchers(scope.clone(), Entries::new(entries, not_found), watcher_stream);

    test_server_client(fio::OpenFlags::RIGHT_READABLE, root, |root| async move {
        let mask = fio::WatchMask::ADDED | fio::WatchMask::REMOVED;
        let watcher_client = assert_watch!(root, mask);

        watcher_sender
            .unbounded_send(WatcherEvent::Added(vec!["four".to_string()]))
            .expect("watcher_sender.send() failed");

        assert_watcher_one_message_watched_events!(watcher_client, { ADDED, "four" });

        watcher_sender.close_channel();

        assert_channel_closed!(watcher_client);
        assert_close!(root);
    })
    .exec(exec)
    .run();
}
