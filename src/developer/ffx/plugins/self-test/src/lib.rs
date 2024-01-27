// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::test::*;
use anyhow::Result;
use ffx_selftest_args::SelftestCommand;
use fho::{FfxMain, FfxTool, SimpleWriter};
use std::time::Duration;

mod component;
mod config;
mod daemon;
mod debug;
mod experiment;
// mod log;
mod target;
mod test;

#[derive(FfxTool)]
pub struct SelfTestTool {
    #[command]
    cmd: SelftestCommand,
}

fho::embedded_plugin!(SelfTestTool);

#[async_trait::async_trait(?Send)]
impl FfxMain for SelfTestTool {
    type Writer = SimpleWriter;

    async fn main(self, _writer: Self::Writer) -> fho::Result<()> {
        selftest(self.cmd).await.map_err(Into::into)
    }
}

pub async fn selftest(cmd: SelftestCommand) -> Result<()> {
    let default_tests = tests![
        test_isolated,
        config::test_env,
        config::test_env_get_global,
        config::test_get_unknown_key,
        config::test_set_then_get,
        experiment::test_not_enabled,
        experiment::test_enabled,
        daemon::test_echo,
        daemon::test_config_flag,
        daemon::test_stop,
        daemon::test_no_autostart,
        target::test_get_ssh_address_timeout,
        target::test_manual_add_get_ssh_address,
        target::test_manual_add_get_ssh_address_late_add,
    ];

    let mut target_tests = tests![
        // TODO(bbosak): re-enable once proactive-logging is disabled (fxb/125487)
        // log::include_log::test_log_run_normal,
        component::include_target::test_list,
        debug::include_target::test_debug_run_crasher,
        debug::include_target::test_debug_limbo,
        target::include_target::test_list,
        target::include_target::test_get_ssh_address_includes_port,
        target::include_target::test_target_show
    ];

    let mut tests = default_tests;
    if cmd.include_target {
        tests.append(&mut target_tests);
    }
    if let Some(filter) = cmd.filter {
        tests.retain(|test| test.name.contains(&filter));
    }

    run(tests, Duration::from_secs(cmd.timeout), Duration::from_secs(cmd.case_timeout)).await
}

async fn test_isolated() -> Result<()> {
    let isolate = new_isolate("isolated").await?;
    let out = isolate.ffx(&["config", "get", "test.is-isolated"]).await?;
    assert_eq!(out.stdout, "true\n");

    Ok(())
}
