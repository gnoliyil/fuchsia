// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::test::*;
use anyhow::{ensure, Context, Result};
use errors::ffx_bail;
use fuchsia_async::{unblock, TimeoutExt, Timer};
use regex::Regex;
use std::{
    fs::{create_dir_all, File},
    io::{BufRead, BufReader, Read, Write},
    path::Path,
    process::Stdio,
    sync::mpsc::channel,
    time::Duration,
};

pub mod include_target {
    use super::*;

    pub(crate) async fn test_debug_run_crasher() -> Result<()> {
        // If the test is running on CI/CQ bots, it's isolated with only files listed as test_data
        // available. We have added zxdb and zxdb-meta.json in ffx-e2e-test-data but we have to
        // also provide an index file at host_x64/sdk/manifest/host_tools.internal.
        // Only when invoked from ffx-e2e-with-target.sh we could get sdk.root=.
        if ffx_config::get::<String, _>("sdk.root").await.unwrap_or_default() == "." {
            ensure!(cfg!(target_arch = "x86_64"), "The test only supports x86_64 for now.");
            let manifest_file = Path::new("sdk/manifest/core");
            if !manifest_file.exists() {
                create_dir_all(manifest_file.parent().unwrap())?;
                File::create(manifest_file)?.write_all(br#"{
                    "atoms": [{
                        "category": "partner",
                        "deps": [],
                        "files": [
                        {
                            "destination": "tools/x64/zxdb",
                            "source": "host_x64/zxdb"
                        },
                        {
                            "destination": "tools/x64/zxdb-meta.json",
                            "source": "host_x64/gen/src/developer/debug/zxdb/zxdb_sdk.meta.json"
                        }
                        ],
                        "gn-label": "//src/developer/debug/zxdb:zxdb_sdk(//build/toolchain:host_x64)",
                        "id": "sdk://tools/x64/zxdb",
                        "meta": "tools/x64/zxdb-meta.json",
                        "plasa": [],
                        "type": "host_tool"
                    }],
                    "ids": []
                }"#)?;
            }
        }

        let target = get_target_nodename().await?;
        let sdk = ffx_config::global_env_context()
            .context("loading global environment context")?
            .get_sdk()
            .await?;
        let isolate = new_isolate("debug-run-crasher").await?;
        isolate.start_daemon().await?;
        let mut config = "sdk.root=".to_owned();
        config.push_str(sdk.get_path_prefix().to_str().unwrap());
        if sdk.get_version() == &sdk::SdkVersion::InTree {
            config.push_str(",sdk.type=in-tree");
        }
        let mut child = isolate
            .ffx_cmd(&[
                "--target",
                &target,
                "--config",
                &config,
                "debug",
                "connect",
                "-e",
                "run /boot/bin/crasher",
            ])
            .await?
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()?;

        let (sender, receiver) = channel();

        let res: Result<_> = unblock(move || {
            let mut stdout_reader = BufReader::new(child.stdout.take().unwrap());

            loop {
                let mut line = String::new();
                let size = stdout_reader.read_line(&mut line)?;
                if size == 0 {
                    let mut stderr = String::new();
                    match BufReader::new(child.stderr.take().unwrap()).read_to_string(&mut stderr) {
                        Ok(_) => ffx_bail!("Unexpected EOF (stderr = {:?})", stderr),
                        Err(e) => ffx_bail!("Unexpected EOF (stderr unreported: {:?})", e),
                    }
                }

                let possible_patterns = [
                    "Page fault writing address 0x0", // x64
                    "Page fault reading address 0x0", // x64-asan
                    "Data fault writing address 0x0", // arm64
                    "Data fault reading address 0x0", // arm64-asan
                ];
                if possible_patterns.iter().any(|pat| line.contains(pat)) {
                    child.wait()?; // close the stdin and the process should exit.
                    return Ok(());
                }
                sender.send(line)?;
            }
        })
        // ffx self-test has a per-case timeout of 12 seconds.
        .on_timeout(Duration::from_secs(11), || ffx_bail!("timeout after 11s"))
        .await;
        res.map_err(|e| {
            while let Ok(line) = receiver.recv() {
                eprint!("{}", line);
            }
            e
        })?;
        Ok(())
    }

    pub(crate) async fn test_debug_limbo() -> Result<()> {
        // This test depends on //src/developer/forensics/crasher which is listed in
        // //src/developer/ffx:ffx-e2e-with-target.
        let isolate = new_isolate("debug-limbo").await?;
        isolate.start_daemon().await?;

        let target = get_target_nodename().await?;

        // Ensure limbo is active and clean.
        let output = isolate.ffx(&["--target", &target, "debug", "limbo", "disable"]).await?;
        assert!(output.status.success(), "{:?}", output);
        let output = isolate.ffx(&["--target", &target, "debug", "limbo", "enable"]).await?;
        assert!(output.status.success(), "{:?}", output);

        // Run crasher.
        let output = isolate
            .ffx(&[
                "--target",
                &target,
                "component",
                "run",
                "--recreate",
                "/core/ffx-laboratory:cpp_crasher",
                "fuchsia-pkg://fuchsia.com/crasher#meta/cpp_crasher.cm",
            ])
            .await?;
        assert!(output.status.success(), "{:?}", output);

        // "cpp_crasher.cm" should be in the limbo.
        // Since we cannot control how long the crasher will crash, do some retries here.
        let mut retries = 5;
        let re = Regex::new(r"cpp_crasher\.cm \(pid: (\d+)\)").unwrap();
        let pid = loop {
            let output = isolate.ffx(&["--target", &target, "debug", "limbo", "list"]).await?;
            assert!(output.status.success(), "{:?}", output);
            match re.captures(&output.stdout) {
                None => {
                    assert!(retries > 0, "\"cpp_crasher.cm\" didn't appear in the limbo :(");
                    retries -= 1;
                    Timer::new(Duration::from_secs(1)).await;
                }
                Some(capture) => {
                    break capture.get(1).unwrap().as_str().to_owned();
                }
            }
        };

        // Release the crasher.
        let output = isolate.ffx(&["--target", &target, "debug", "limbo", "release", &pid]).await?;
        assert!(output.status.success(), "{:?}", output);

        // The output should not contain cpp_crasher.cm.
        let output = isolate.ffx(&["--target", &target, "debug", "limbo", "list"]).await?;
        assert!(output.status.success(), "{:?}", output);
        assert!(re.find(&output.stdout).is_none());

        // Disable process limbo.
        let output = isolate.ffx(&["--target", &target, "debug", "limbo", "disable"]).await?;
        assert!(output.status.success(), "{:?}", output);

        // Destroy cpp_crasher, otherwise the component will stay there even if it's dead.
        let output = isolate
            .ffx(&["--target", &target, "component", "destroy", "/core/ffx-laboratory:cpp_crasher"])
            .await?;
        assert!(output.status.success(), "{:?}", output);

        Ok(())
    }
}
