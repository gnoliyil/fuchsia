// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    fidl::endpoints::create_endpoints,
    fidl_fuchsia_io as fio,
    fuchsia_zircon::{self as zx, HandleBased},
    std::sync::Arc,
    syncio::Zxio,
    vfs::{
        directory::entry::DirectoryEntry, execution_scope::ExecutionScope, path::Path,
        pseudo_directory, symlink::Symlink,
    },
};

// This needs more than one thread because zxio makes synchronous FIDL calls.
#[fuchsia::test(threads = 2)]
async fn test_symlink() {
    struct TestSymlink;

    impl Symlink for TestSymlink {
        fn read_target(&self) -> Result<Vec<u8>, zx::Status> {
            Ok(b"target".to_vec())
        }
    }

    struct ErrorSymlink;
    impl Symlink for ErrorSymlink {
        fn read_target(&self) -> Result<Vec<u8>, zx::Status> {
            Err(zx::Status::IO)
        }
    }

    let dir = pseudo_directory! {
        "symlink" => Arc::new(TestSymlink),
        "error_symlink" => Arc::new(ErrorSymlink),
    };

    let (dir_client, dir_server) = create_endpoints();
    let scope = ExecutionScope::new();
    dir.open(
        scope,
        fio::OpenFlags::RIGHT_READABLE | fio::OpenFlags::DIRECTORY,
        Path::dot(),
        dir_server,
    );

    let dir_zxio = Zxio::create(dir_client.into_channel().into_handle()).expect("create failed");
    let symlink_zxio =
        dir_zxio.open(fio::OpenFlags::RIGHT_READABLE, "symlink").expect("open failed");
    assert_eq!(symlink_zxio.read_link().expect("read_link failed"), b"target");

    assert_eq!(
        dir_zxio.open(fio::OpenFlags::RIGHT_READABLE, "error_symlink").expect_err("open succeeded"),
        zx::Status::IO
    );
}
