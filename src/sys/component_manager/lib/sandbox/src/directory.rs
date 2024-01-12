// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
use core::fmt;
use fidl::endpoints::{create_endpoints, ClientEnd};
use fidl_fuchsia_component_sandbox as fsandbox;
use fidl_fuchsia_io as fio;
use fuchsia_async as fasync;
use fuchsia_zircon::{self as zx, AsHandleRef};
use futures::{future::BoxFuture, FutureExt};
use std::sync::{Arc, Mutex};
use vfs::execution_scope::ExecutionScope;

use crate::{registry, Capability, Open};

/// A capability that is a `fuchsia.io` directory.
///
/// The directory may optionally be backed by a future that serves its contents.
#[derive(Capability)]
pub struct Directory {
    /// The FIDL representation of this [Directory].
    ///
    /// Invariant: Always Some when the Directory is outside of the registry.
    client_end: Option<ClientEnd<fio::DirectoryMarker>>,

    // The mutex makes the Directory implement `Sync`.
    future: Arc<Mutex<Option<BoxFuture<'static, ()>>>>,
}

impl Directory {
    /// Create a new [Directory] capability.
    ///
    /// Arguments:
    ///
    /// * `client_end` - A `fuchsia.io/Directory` client endpoint.
    /// * `future` - If present, the future will serve the contents in the directory.
    pub fn new(
        client_end: ClientEnd<fio::DirectoryMarker>,
        future: Option<BoxFuture<'static, ()>>,
    ) -> Self {
        Directory { client_end: Some(client_end), future: Arc::new(Mutex::new(future)) }
    }

    /// Create a new [Directory] capability that will open entries using the [Open] capability.
    ///
    /// Arguments:
    ///
    /// * `open_flags` - The flags that will be used to open a new connection from the [Open]
    ///   capability.
    pub fn from_open(open: Open, open_flags: fio::OpenFlags) -> Self {
        let scope = ExecutionScope::new();
        let (client_end, server_end) = create_endpoints::<fio::DirectoryMarker>();
        // If this future is dropped, stop serving the connection.
        let guard = scopeguard::guard(scope.clone(), move |scope| {
            scope.shutdown();
        });
        let fut = async move {
            let _guard = guard;
            // Wait for the client endpoint to be written or closed. These are the only two
            // operations the client could do that warrants our attention.
            let server_end = fasync::Channel::from_channel(server_end.into_channel());
            let on_signal_fut = fasync::OnSignals::new(
                &server_end,
                zx::Signals::CHANNEL_READABLE | zx::Signals::CHANNEL_PEER_CLOSED,
            );
            let signals = on_signal_fut.await.unwrap();
            if signals & zx::Signals::CHANNEL_READABLE != zx::Signals::NONE {
                open.open(
                    scope.clone(),
                    open_flags,
                    vfs::path::Path::dot(),
                    server_end.into_zx_channel().into(),
                );
            }
            scope.wait().await;
        }
        .boxed();

        Self::new(client_end, Some(fut))
    }

    /// Sets this directory's client end to the provided one.
    ///
    /// This should only be used to put a remoted client end back into the Directory
    /// after it is removed from the registry.
    pub(crate) fn set_client_end(&mut self, client_end: ClientEnd<fio::DirectoryMarker>) {
        self.client_end = Some(client_end)
    }
}

impl fmt::Debug for Directory {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Directory").field("client_end", &self.client_end).finish()
    }
}

impl Clone for Directory {
    fn clone(&self) -> Self {
        // Call `fuchsia.io/Directory.Clone` without converting the ClientEnd into a proxy.
        // This is necessary because we the conversion consumes the ClientEnd, but we can't take
        // it out of non-mut `&self`.
        let (clone_client_end, clone_server_end) = zx::Channel::create();
        let raw_handle = self.client_end.as_ref().unwrap().as_handle_ref().raw_handle();
        // SAFETY: the channel is forgotten at the end of scope so it is not double closed.
        unsafe {
            let borrowed: zx::Channel = zx::Handle::from_raw(raw_handle).into();
            let directory = fio::DirectorySynchronousProxy::new(borrowed);
            let _ = directory.clone(fio::OpenFlags::CLONE_SAME_RIGHTS, clone_server_end.into());
            std::mem::forget(directory.into_channel());
        }
        let client_end: ClientEnd<fio::DirectoryMarker> = clone_client_end.into();
        Self { client_end: Some(client_end), future: self.future.clone() }
    }
}

impl Capability for Directory {}

impl From<ClientEnd<fio::DirectoryMarker>> for Directory {
    fn from(client_end: ClientEnd<fio::DirectoryMarker>) -> Self {
        Directory { client_end: Some(client_end), future: Arc::new(Mutex::new(None)) }
    }
}

impl From<Directory> for ClientEnd<fio::DirectoryMarker> {
    /// Returns the `fuchsia.io.Directory` client stored in this Directory, taking it out,
    /// and moves the capability into the registry.
    ///
    /// The client end is put back when the Directory is removed from the registry.
    fn from(mut directory: Directory) -> Self {
        let client_end = directory.client_end.take().expect("BUG: missing client end");

        // Move this capability into the registry.
        let koid = client_end.get_koid().unwrap();
        let future = directory.future.lock().unwrap().take();
        if let Some(fut) = future {
            let task = fasync::Task::spawn(fut);
            registry::insert_with_task(Box::new(directory), koid, task);
        } else {
            registry::insert(Box::new(directory), koid);
        }

        client_end
    }
}

impl From<Directory> for fsandbox::Capability {
    fn from(directory: Directory) -> Self {
        Self::Directory(directory.into())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use fidl::endpoints::{create_endpoints, ClientEnd, ServerEnd};
    use fidl_fuchsia_io as fio;
    use fuchsia_zircon as zx;
    use futures::channel::{mpsc, oneshot};
    use futures::StreamExt;
    use lazy_static::lazy_static;
    use std::sync::Arc;
    use std::sync::Mutex;
    use test_util::Counter;
    use vfs::{
        directory::entry::DirectoryEntry,
        execution_scope::ExecutionScope,
        path::Path,
        pseudo_directory,
        remote::{remote_boxed_with_type, RoutingFn},
    };

    fn serve_vfs_dir(root: Arc<impl DirectoryEntry>) -> ClientEnd<fio::DirectoryMarker> {
        let scope = ExecutionScope::new();
        let (client, server) = create_endpoints::<fio::DirectoryMarker>();
        root.open(
            scope.clone(),
            fio::OpenFlags::RIGHT_READABLE,
            vfs::path::Path::dot(),
            ServerEnd::new(server.into_channel()),
        );
        client
    }

    #[fuchsia::test]
    async fn test_remote_from_open() {
        let (open_tx, open_rx) = oneshot::channel::<()>();
        let open_tx = Mutex::new(Some(open_tx));

        let open = Open::new(
            move |_scope: ExecutionScope,
                  flags: fio::OpenFlags,
                  relative_path: Path,
                  _server_end: zx::Channel| {
                assert_eq!(relative_path.into_string(), "");
                assert_eq!(flags, fio::OpenFlags::DIRECTORY);
                open_tx.lock().unwrap().take().unwrap().send(()).unwrap();
            },
            fio::DirentType::Directory,
        );

        let directory = open.into_directory(fio::OpenFlags::DIRECTORY);
        let client_end: ClientEnd<fio::DirectoryMarker> = directory.into();
        zx::Channel::from(client_end)
            .write(&[1], &mut [])
            .expect("should be able to write to the client endpoint");

        open_rx.await.unwrap();
    }

    #[fuchsia::test]
    async fn test_remote_from_open_not_used_if_not_written_to() {
        lazy_static! {
            static ref OPEN_COUNT: Counter = Counter::new(0);
        }

        let open = Open::new(
            move |_scope: ExecutionScope,
                  flags: fio::OpenFlags,
                  relative_path: Path,
                  server_end: zx::Channel| {
                assert_eq!(relative_path.into_string(), "");
                assert_eq!(flags, fio::OpenFlags::DIRECTORY);
                OPEN_COUNT.inc();
                drop(server_end);
            },
            fio::DirentType::Directory,
        );

        assert_eq!(OPEN_COUNT.get(), 0);
        let directory = open.into_directory(fio::OpenFlags::DIRECTORY);
        let client_end: ClientEnd<fio::DirectoryMarker> = directory.into();
        drop(client_end);
        assert_eq!(OPEN_COUNT.get(), 0);
    }

    #[fuchsia::test]
    async fn test_clone() {
        let (open_tx, mut open_rx) = mpsc::channel::<()>(1);
        let open_tx = Arc::new(Mutex::new(open_tx));

        let open_tx = open_tx.clone();
        let routing_fn: RoutingFn = Box::new(move |_scope, flags, relative_path, _server_end| {
            assert_eq!(relative_path.into_string(), "");
            assert_eq!(flags, fio::OpenFlags::DIRECTORY);
            open_tx.lock().unwrap().try_send(()).unwrap();
        });
        let fs = pseudo_directory! {
            "foo" => remote_boxed_with_type(routing_fn, fio::DirentType::Directory),
        };

        // Create a Directory capability, and a clone.
        let dir = Directory::from(serve_vfs_dir(fs));
        let dir_clone = dir.clone();

        // Open the original directory.
        let client_end: ClientEnd<fio::DirectoryMarker> = dir.into();
        let dir_proxy = client_end.into_proxy().unwrap();
        let (_client_end, server_end) = create_endpoints::<fio::NodeMarker>();
        dir_proxy
            .open(fio::OpenFlags::DIRECTORY, fio::ModeType::empty(), "foo", server_end)
            .unwrap();

        // The Open capability should receive the Open request.
        open_rx.next().await.unwrap();

        // Open the clone.
        let client_end: ClientEnd<fio::DirectoryMarker> = dir_clone.into();
        let clone_proxy = client_end.into_proxy().unwrap();
        let (_client_end, server_end) = create_endpoints::<fio::NodeMarker>();
        clone_proxy
            .open(fio::OpenFlags::DIRECTORY, fio::ModeType::empty(), "foo", server_end)
            .unwrap();

        // The Open capability should receive the Open request from the clone.
        open_rx.next().await.unwrap();
    }
}
