// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! A node which forwards open requests to a remote fuchsia.io server.

#[cfg(test)]
mod tests;

use crate::{
    directory::entry::{DirectoryEntry, EntryInfo},
    execution_scope::ExecutionScope,
    path::Path,
    service::connection::io1::Connection,
};

use {
    fidl::{self, endpoints::ServerEnd},
    fidl_fuchsia_io as fio,
    std::sync::Arc,
};

/// The type for the callback function used to create new connections to the remote object. The
/// arguments mirror DirectoryEntry::open.
pub type RoutingFn = Box<
    dyn Fn(ExecutionScope, fio::OpenFlags, u32, Path, ServerEnd<fio::NodeMarker>) + Send + Sync,
>;

/// Create a new [`Remote`] node that forwards requests to the provided [`RoutingFn`]. This routing
/// function is called once per open request. The dirent type is set to the provided
/// `dirent_type`, which should be one of the `DIRENT_TYPE_*` values defined in fuchsia.io.
pub fn remote_boxed_with_type(open: RoutingFn, dirent_type: fio::DirentType) -> Arc<Remote> {
    Arc::new(Remote { open, dirent_type })
}

/// Create a new [`Remote`] node that forwards open requests to the provided [`RoutingFn`]. This
/// routing function is called once per open request. The dirent type is set as
/// `DirentType::Unknown`. If the remote node is a known `DIRENT_TYPE_*` type, you may wish to use
/// [`remote_boxed_with_type`] instead.
pub fn remote_boxed(open: RoutingFn) -> Arc<Remote> {
    remote_boxed_with_type(open, fio::DirentType::Unknown)
}

/// Create a new [`Remote`] node that forwards open requests to the provided callback. This routing
/// function is called once per open request. The dirent type is set as `DirentType::Unknown`. If
/// the remote node is a known `DIRENT_TYPE_*` type, you may wish to use [`remote_boxed_with_type`]
/// instead.
pub fn remote<Open>(open: Open) -> Arc<Remote>
where
    Open: Fn(ExecutionScope, fio::OpenFlags, u32, Path, ServerEnd<fio::NodeMarker>)
        + Send
        + Sync
        + 'static,
{
    remote_boxed(Box::new(open))
}

/// Create a new [`Remote`] node that forwards open requests to the provided [`DirectoryProxy`],
/// effectively handing off the handling of any further requests to the remote fidl server.
pub fn remote_dir(dir: fio::DirectoryProxy) -> Arc<Remote> {
    remote_boxed_with_type(
        Box::new(move |_scope, flags, mode, path, server_end| {
            let _ = dir.open(flags, mode, path.as_ref(), server_end);
        }),
        fio::DirentType::Directory,
    )
}

/// A Remote node is a node which forwards most open requests to another entity. The forwarding is
/// done by calling a routing function of type [`RoutingFn`] provided at the time of construction.
/// The remote node itself doesn't do any flag validation when forwarding the open call.
///
/// When a remote node is opened with OPEN_FLAG_NO_REMOTE, a connection to this local node is
/// created instead.
pub struct Remote {
    open: RoutingFn,
    dirent_type: fio::DirentType,
}

impl Remote {
    /// Open this node as a fuchsia.io/Node. This reuses the service connection implementation as
    /// they are functionally equivalent in this scenario.
    fn open_as_node(
        self: Arc<Self>,
        scope: ExecutionScope,
        mut flags: fio::OpenFlags,
        mode: u32,
        server_end: ServerEnd<fio::NodeMarker>,
    ) {
        // We are reusing the service connection implementation, but to get it to work, we must
        // strip the OPEN_FLAG_NO_REMOTE flag (since that isn't permitted for services).  Our
        // implementation also only allows access if both OPEN_RIGHT_READABLE and
        // OPEN_WRITE_WRITABLE is set, or it's OPEN_FLAG_NODE_REFERENCE, so, for now, we hack the
        // flags to be OPEN_FLAG_NODE_REFERENCE which will cause the rights to be ignored.
        if flags.intersects(fio::OpenFlags::NO_REMOTE) {
            flags &= !fio::OpenFlags::NO_REMOTE;
            flags |= fio::OpenFlags::NODE_REFERENCE;
        }
        Connection::create_connection(scope, flags, mode, server_end);
    }

    fn open_as_remote(
        self: Arc<Self>,
        scope: ExecutionScope,
        flags: fio::OpenFlags,
        mode: u32,
        path: Path,
        server_end: ServerEnd<fio::NodeMarker>,
    ) {
        // There is no flag validation to do here. All flags are either handled by the initial
        // connection that forwarded this open request (if it exists) or the remote node.
        (self.open)(scope, flags, mode, path, server_end);
    }
}

impl DirectoryEntry for Remote {
    fn open(
        self: Arc<Self>,
        scope: ExecutionScope,
        flags: fio::OpenFlags,
        mode: u32,
        path: Path,
        server_end: ServerEnd<fio::NodeMarker>,
    ) {
        if flags.intersects(fio::OpenFlags::NO_REMOTE) && path.is_empty() {
            self.open_as_node(scope, flags, mode, server_end);
        } else {
            self.open_as_remote(scope, flags, mode, path, server_end);
        }
    }

    fn entry_info(&self) -> EntryInfo {
        EntryInfo::new(fio::INO_UNKNOWN, self.dirent_type)
    }
}
