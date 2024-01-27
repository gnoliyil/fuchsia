// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Common utilities used by directory related tests.
//!
//! Most assertions are macros as they need to call async functions themselves.  As a typical test
//! will have multiple assertions, it save a bit of typing to write `assert_something!(arg)`
//! instead of `assert_something(arg).await`.

#[doc(hidden)]
pub mod reexport {
    pub use {
        fidl, fidl_fuchsia_io as fio,
        fuchsia_async::Channel,
        fuchsia_zircon::{MessageBuf, Status},
    };
}

use crate::{
    directory::entry::DirectoryEntry,
    test_utils::run::{self, AsyncServerClientTestParams},
};

use {
    byteorder::{LittleEndian, WriteBytesExt},
    fidl_fuchsia_io as fio,
    futures::Future,
    std::{convert::TryInto as _, io::Write, sync::Arc},
};

pub use run::{run_client, test_client};

/// A thin wrapper around [`run::run_server_client()`] that sets the `Marker` to be
/// [`DirectoryMarker`], and providing explicit type for the `get_client` closure argument.  This
/// makes it possible for the caller not to provide explicit types.
pub fn run_server_client<GetClientRes>(
    flags: fio::OpenFlags,
    server: Arc<dyn DirectoryEntry>,
    get_client: impl FnOnce(fio::DirectoryProxy) -> GetClientRes,
) where
    GetClientRes: Future<Output = ()>,
{
    run::run_server_client::<fio::DirectoryMarker, _, _>(flags, server, get_client)
}

/// A thin wrapper around [`run::test_server_client()`] that sets the `Marker` to be
/// [`DirectoryMarker`], and providing explicit type for the `get_client` closure argument.  This
/// makes it possible for the caller not to provide explicit types.
pub fn test_server_client<'test_refs, GetClientRes>(
    flags: fio::OpenFlags,
    server: Arc<dyn DirectoryEntry>,
    get_client: impl FnOnce(fio::DirectoryProxy) -> GetClientRes + 'test_refs,
) -> AsyncServerClientTestParams<'test_refs, fio::DirectoryMarker>
where
    GetClientRes: Future<Output = ()> + 'test_refs,
{
    run::test_server_client::<fio::DirectoryMarker, _, _>(flags, server, get_client)
}

/// A helper to build the "expected" output for a `ReadDirents` call from the Directory protocol in
/// fuchsia.io.
pub struct DirentsSameInodeBuilder {
    expected: Vec<u8>,
    inode: u64,
}

impl DirentsSameInodeBuilder {
    pub fn new(inode: u64) -> Self {
        DirentsSameInodeBuilder { expected: vec![], inode }
    }

    pub fn add(&mut self, type_: fio::DirentType, name: &[u8]) -> &mut Self {
        assert!(
            name.len() <= fio::MAX_FILENAME as usize,
            "Expected entry name should not exceed MAX_FILENAME ({}) bytes.\n\
             Got: {:?}\n\
             Length: {} bytes",
            fio::MAX_FILENAME,
            name,
            name.len()
        );

        self.expected.write_u64::<LittleEndian>(self.inode).unwrap();
        self.expected.write_u8(name.len().try_into().unwrap()).unwrap();
        self.expected.write_u8(type_.into_primitive()).unwrap();
        self.expected.write_all(name).unwrap();

        self
    }

    pub fn into_vec(self) -> Vec<u8> {
        self.expected
    }
}

/// Calls `rewind` on the provided `proxy`, checking that the result status is Status::OK.
#[macro_export]
macro_rules! assert_rewind {
    ($proxy:expr) => {{
        use $crate::directory::test_utils::reexport::Status;

        let status = $proxy.rewind().await.expect("rewind failed");
        assert_eq!(Status::from_raw(status), Status::OK);
    }};
}

/// Opens the specified path as a file and checks its content.  Also see all the `assert_*` macros
/// in `../test_utils.rs`.
#[macro_export]
macro_rules! open_as_file_assert_content {
    ($proxy:expr, $flags:expr, $path:expr, $expected_content:expr) => {{
        let file = open_get_file_proxy_assert_ok!($proxy, $flags, $path);
        assert_read!(file, $expected_content);
        assert_close!(file);
    }};
}

/// Opens the specified path as a VMO file and checks its content.  Also see all the `assert_*`
/// macros in `../test_utils.rs`.
#[macro_export]
macro_rules! open_as_vmo_file_assert_content {
    ($proxy:expr, $flags:expr, $path:expr, $expected_content:expr) => {{
        let file = open_get_vmo_file_proxy_assert_ok!($proxy, $flags, $path);
        assert_read!(file, $expected_content);
        assert_close!(file);
    }};
}

#[macro_export]
macro_rules! assert_watch {
    ($proxy:expr, $mask:expr) => {{
        use $crate::directory::test_utils::reexport::{fidl, Channel, Status};

        let (client, server) = fidl::endpoints::create_endpoints();

        let status = $proxy.watch($mask, 0, server).await.expect("watch failed");
        assert_eq!(Status::from_raw(status), Status::OK);

        Channel::from_channel(client.into_channel()).unwrap()
    }};
}

#[macro_export]
macro_rules! assert_watch_err {
    ($proxy:expr, $mask:expr, $expected_status:expr) => {{
        use $crate::directory::test_utils::reexport::{fidl, Status};

        let (_client, server) = fidl::endpoints::create_endpoints();

        let status = $proxy.watch($mask, 0, server).await.expect("watch failed");
        assert_eq!(Status::from_raw(status), $expected_status);
    }};
}

#[macro_export]
macro_rules! assert_watcher_one_message_watched_events {
    ($watcher:expr, $( { $type:tt, $name:expr $(,)* } ),* $(,)*) => {{
        #[allow(unused)]
        use $crate::directory::test_utils::reexport::{MessageBuf, fio::WatchEvent};
        use std::convert::TryInto as _;

        let mut buf = MessageBuf::new();
        $watcher.recv_msg(&mut buf).await.unwrap();

        let (bytes, handles) = buf.split();
        assert_eq!(
            handles.len(),
            0,
            "Received buffer with handles.\n\
             Handle count: {}\n\
             Buffer: {:X?}",
            handles.len(),
            bytes
        );

        let expected = &mut vec![];
        $({
            let type_ = assert_watcher_one_message_watched_events!(@expand_event_type $type);
            let name = Vec::<u8>::from($name);
            assert!(name.len() <= std::u8::MAX as usize);

            expected.push(type_.into_primitive());
            expected.push(name.len().try_into().unwrap());
            expected.extend_from_slice(&name);
        })*

        assert!(bytes == *expected,
                "Received buffer does not match the expectation.\n\
                 Expected: {:X?}\n\
                 Received: {:X?}\n\
                 Expected as UTF-8 lossy: {:?}\n\
                 Received as UTF-8 lossy: {:?}",
                *expected, bytes,
                String::from_utf8_lossy(expected), String::from_utf8_lossy(&bytes));
    }};

    (@expand_event_type EXISTING) => { fio::WatchEvent::Existing };
    (@expand_event_type IDLE) => { fio::WatchEvent::Idle };
    (@expand_event_type ADDED) => { fio::WatchEvent::Added };
    (@expand_event_type REMOVED) => { fio::WatchEvent::Removed };
}
