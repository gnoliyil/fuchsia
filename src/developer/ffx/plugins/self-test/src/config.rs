// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::{assert, assert_eq, test::*};
use anyhow::*;

pub(crate) async fn test_env() -> Result<()> {
    let isolate = new_isolate("config-env").await?;
    let out = isolate.ffx(&["config", "env"]).await?;

    assert!(out.status.success());
    assert!(out.stdout.contains("Environment:"));
    assert!(out.stdout.contains("User:"));
    assert!(out.stdout.contains("Build:"));
    assert!(out.stdout.contains("Global:"));

    Ok(())
}

pub(crate) async fn test_env_get_global() -> Result<()> {
    let isolate = new_isolate("config-env-get-global").await?;
    let out = isolate.ffx(&["config", "env", "get", "global"]).await?;

    assert!(out.status.success());
    assert_eq!(out.stdout.trim(), "Global: none");

    Ok(())
}

pub(crate) async fn test_get_unknown_key() -> Result<()> {
    let isolate = new_isolate("config-get-unknown-key").await?;
    let out = isolate.ffx(&["config", "get", "this-key-SHOULD-NOT-exist"]).await?;

    assert!(out.stdout.is_empty());
    assert!(!out.stderr.is_empty());
    assert!(!out.status.success());
    assert_eq!(out.status.code().unwrap(), 2);

    Ok(())
}

pub(crate) async fn test_set_then_get() -> Result<()> {
    let isolate = new_isolate("config-set-then-get").await?;
    let value = "42";

    let out = isolate.ffx(&["config", "set", "test-unique-key", value]).await?;

    assert!(out.stdout.is_empty());
    assert!(out.status.success());

    let out_get = isolate.ffx(&["config", "get", "test-unique-key"]).await?;

    assert_eq!(out_get.stdout.trim(), value);
    assert!(out_get.status.success());

    Ok(())
}
