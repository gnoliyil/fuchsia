// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use fidl_fuchsia_dash as fdash;
use fuchsia_component::client::connect_to_protocol;
use fuchsia_zircon as zx;

#[fuchsia::test]
pub async fn unknown_tools_package() {
    let (_stdio, stdio_server) = zx::Socket::create(zx::SocketOpts::STREAM).unwrap();

    let launcher = connect_to_protocol::<fdash::LauncherMarker>().unwrap();

    let v = vec!["fuchsia-pkg://fuchsia.com/bar".to_string()];
    let urls: &mut dyn ExactSizeIterator<Item = &str> = &mut v.iter().map(|s| s.as_str());
    let err = launcher
        .launch_with_socket(
            ".",
            stdio_server,
            urls,
            None,
            fdash::DashNamespaceLayout::NestAllInstanceDirs,
        )
        .await
        .unwrap()
        .unwrap_err();

    assert_eq!(err, fdash::LauncherError::ToolsBinaryRead);
}

#[fuchsia::test]
pub async fn bad_moniker() {
    let (_stdio, stdio_server) = zx::Socket::create(zx::SocketOpts::STREAM).unwrap();

    let launcher = connect_to_protocol::<fdash::LauncherMarker>().unwrap();

    // Give a string that won't parse correctly as a moniker.
    let err = launcher
        .launch_with_socket(
            "!@#$%^&*(",
            stdio_server,
            &mut std::iter::empty(),
            None,
            fdash::DashNamespaceLayout::NestAllInstanceDirs,
        )
        .await
        .unwrap()
        .unwrap_err();
    assert_eq!(err, fdash::LauncherError::BadMoniker);
}

#[fuchsia::test]
pub async fn instance_not_found() {
    let (_stdio, stdio_server) = zx::Socket::create(zx::SocketOpts::STREAM).unwrap();

    let launcher = connect_to_protocol::<fdash::LauncherMarker>().unwrap();

    // Give a moniker to an instance that does not exist.
    let err = launcher
        .launch_with_socket(
            "./does_not_exist",
            stdio_server,
            &mut std::iter::empty(),
            None,
            fdash::DashNamespaceLayout::NestAllInstanceDirs,
        )
        .await
        .unwrap()
        .unwrap_err();
    assert_eq!(err, fdash::LauncherError::InstanceNotFound);
}

#[fuchsia::test]
pub async fn bad_url() {
    let (_stdio, stdio_server) = zx::Socket::create(zx::SocketOpts::STREAM).unwrap();

    let launcher = connect_to_protocol::<fdash::LauncherMarker>().unwrap();

    let v = vec!["fuchsia-pkg://fuchsia.com/!@#$%^&*(".to_string()];
    let urls: &mut dyn ExactSizeIterator<Item = &str> = &mut v.iter().map(|s| s.as_str());
    let err = launcher
        .launch_with_socket(
            ".",
            stdio_server,
            urls,
            None,
            fdash::DashNamespaceLayout::NestAllInstanceDirs,
        )
        .await
        .unwrap()
        .unwrap_err();
    assert_eq!(err, fdash::LauncherError::BadUrl);
}
