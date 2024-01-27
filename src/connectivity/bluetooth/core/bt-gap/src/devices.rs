// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! This module defines a watcher that subscribes to the device filesystem and
//! produces a stream of messages when bt-host devices are added or removed from
//! the system

use fuchsia_bluetooth::constants::HOST_DEVICE_DIR;
use fuchsia_fs::directory::{self as vfs_watcher, WatchEvent, WatchMessage, WatcherStreamError};
use fuchsia_fs::OpenFlags;
use futures::{future, FutureExt, Stream, TryStreamExt};
use std::ffi::OsStr;
use std::path::{Path, PathBuf};
use tracing::{info, warn};

pub enum HostEvent {
    HostAdded(PathBuf),
    HostRemoved(PathBuf),
}

/// Watch the VFS for host devices being added or removed, and produce a stream of HostEvent messages
pub fn watch_hosts() -> impl Stream<Item = Result<HostEvent, WatcherStreamError>> {
    async {
        let directory =
            fuchsia_fs::directory::open_in_namespace(HOST_DEVICE_DIR, OpenFlags::RIGHT_READABLE)
                .unwrap();
        let watcher = vfs_watcher::Watcher::new(&directory)
            .await
            .expect("Cannot open vfs watcher for bt-host device path");
        watcher.try_filter_map(|msg| future::ok(as_host_event(msg)))
    }
    .flatten_stream()
}

fn as_host_event(msg: WatchMessage) -> Option<HostEvent> {
    // This is needed because, in some test scenarios, the vfs_watcher sends a message for the
    // directory's population of the `.` entry. It is reasonable in general, as we'd never want
    // to count the CWD as a "HostEvent".
    if msg.filename == OsStr::new(".") {
        info!("Ignoring spurious host watch event for path \".\"");
        return None;
    }
    let path = Path::new(HOST_DEVICE_DIR).join(&msg.filename);
    match msg.event {
        WatchEvent::EXISTING | WatchEvent::ADD_FILE => Some(HostEvent::HostAdded(path)),
        WatchEvent::REMOVE_FILE => Some(HostEvent::HostRemoved(path)),
        WatchEvent::IDLE => None,
        e => {
            warn!("Unrecognized host watch event: {:?}", e);
            None
        }
    }
}
