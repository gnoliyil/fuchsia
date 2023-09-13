// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::test::*;
use anyhow::*;
use std::time::Duration;

pub(crate) async fn test_get_ssh_address_timeout() -> Result<()> {
    let isolate = new_isolate("target-get-ssh-address-timeout").await?;
    isolate.start_daemon().await?;

    let out = isolate.ffx(&["--target", "noexist", "target", "get-ssh-address", "-t", "1"]).await?;

    ensure!(out.stdout.lines().count() == 0, "stdout is unexpected: {:?}", out);
    // stderr names the target, and says timeout.
    ensure!(out.stderr.contains("noexist"), "stderr is unexpected: {:?}", out);
    ensure!(out.stderr.contains("Timeout"), "stderr is unexpected: {:?}", out);

    Ok(())
}

pub(crate) async fn test_manual_add_get_ssh_address() -> Result<()> {
    let isolate = new_isolate("target-manual-add-get-ssh-address").await?;
    isolate.start_daemon().await?;

    let _ = isolate.ffx(&["target", "add", "--nowait", "[::1]:8022"]).await?;

    let out = isolate.ffx(&["--target", "[::1]:8022", "target", "get-ssh-address"]).await?;

    ensure!(out.stdout.contains("[::1]:8022"), "stdout is unexpected: {:?}", out);
    ensure!(out.stderr.lines().count() == 0, "stderr is unexpected: {:?}", out);
    // TODO: establish a good way to assert against the whole target address.

    Ok(())
}

pub(crate) async fn test_manual_add_get_ssh_address_late_add() -> Result<()> {
    let isolate = new_isolate("target-manual-add-get-ssh-address-late-add").await?;
    isolate.start_daemon().await?;

    let task = isolate.ffx(&["--target", "[::1]:8022", "target", "get-ssh-address", "-t", "10"]);

    // The get-ssh-address should pick up targets added after it has started, as well as before.
    fuchsia_async::Timer::new(Duration::from_millis(500)).await;

    let _ = isolate.ffx(&["target", "add", "--nowait", "[::1]:8022"]).await?;

    let out = task.await?;

    ensure!(out.stdout.contains("[::1]:8022"), "stdout is unexpected: {:?}", out);
    ensure!(out.stderr.lines().count() == 0, "stderr is unexpected: {:?}", out);
    // TODO: establish a good way to assert against the whole target address.

    Ok(())
}

pub mod include_target {
    use super::*;

    // TODO(slgrady): Create tests for "ffx target list" (currently non-existent
    // since the previous test was dependent on discovery), and getting a target
    // by name (currently non-existent since these tests only expect to have an
    // address specified)

    pub(crate) async fn test_get_ssh_address_includes_port() -> Result<()> {
        let isolate = new_isolate("target-get-ssh-address-includes-port").await?;
        isolate.start_daemon().await?;

        let target_nodeaddr = get_target_addr();

        let out = isolate
            .ffx(&["--target", &target_nodeaddr, "target", "get-ssh-address", "-t", "5"])
            .await?;

        ensure!(out.stdout.contains(":22"), "expected stdout to contain ':22', got {:?}", out);
        ensure!(out.stderr.lines().count() == 0, "stderr is unexpected: {:?}", out);
        // TODO: establish a good way to assert against the whole target address.

        Ok(())
    }

    pub(crate) async fn test_target_show() -> Result<()> {
        let isolate = new_isolate("target-show").await?;
        isolate.start_daemon().await?;

        let target_nodeaddr = get_target_addr();

        let out = isolate.ffx(&["--target", &target_nodeaddr, "target", "show"]).await?;

        ensure!(out.status.success(), "status is unexpected: {:?}", out);
        ensure!(!out.stdout.is_empty(), "stdout is unexpectedly empty: {:?}", out);
        ensure!(out.stderr.lines().count() == 0, "stderr is unexpected: {:?}", out);

        Ok(())
    }
}
