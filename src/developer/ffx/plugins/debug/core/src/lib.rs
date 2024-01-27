// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::{anyhow, bail, Context, Result};
use async_trait::async_trait;
use chrono::{Local, TimeZone};
use errors::{ffx_bail, ffx_error};
use ffx_component::rcs::connect_to_realm_query;
use fho::{FfxContext, FfxMain, FfxTool, SimpleWriter};
use fidl_fuchsia_developer_remotecontrol::RemoteControlProxy;
use fidl_fuchsia_io as fio;
use fuchsia_async::unblock;
use fuchsia_fs::directory::{open_directory_no_describe, open_file_no_describe};
use fuchsia_zircon_status::Status;
use futures::StreamExt;
use std::{
    io::{BufRead, Write},
    process::Command,
};
use tempfile::{NamedTempFile, TempPath};

// Must be kept in sync with //src/sys/core/core_component_id_index.json5.
const STORAGE_ID: &str = "eb345fb7dcaa4260ee0c65bb73ef0ec5341b15a4f603f358d6631c4be6bf7080";

#[derive(FfxTool)]
pub struct CoreTool {
    #[command]
    cmd: ffx_debug_core_args::CoreCommand,
    sdk: ffx_config::Sdk,
    rcs: fho::Deferred<RemoteControlProxy>,
}

fho::embedded_plugin!(CoreTool);

#[async_trait(?Send)]
impl FfxMain for CoreTool {
    type Writer = SimpleWriter;
    async fn main(self, _writer: SimpleWriter) -> fho::Result<()> {
        if let Err(e) = symbol_index::ensure_symbol_index_registered(&self.sdk).await {
            eprintln!("ensure_symbol_index_registered failed, error was: {:#?}", e);
        }

        let mut _temp_minidump_path: Option<TempPath> = None; // Drop the file at the end.
        let minidump = match self.cmd.minidump {
            Some(file) => file,
            None => {
                let rcs: RemoteControlProxy = self.rcs.await?;
                _temp_minidump_path = Some(choose_and_copy_remote_minidumps(&rcs).await?);
                _temp_minidump_path.as_ref().unwrap().to_str().unwrap().to_owned()
            }
        };

        let zxdb_path = self.sdk.get_host_tool("zxdb")?;
        let mut args = vec!["--core=".to_owned() + &minidump];
        args.extend(self.cmd.zxdb_args);

        let mut child =
            Command::new(zxdb_path).args(args).spawn().bug_context("Failed to spawn zxdb")?;
        let status =
            unblock(move || child.wait()).await.bug_context("Error waiting for zxdb to exit")?;

        if let Some(exit_code) = status.code() {
            if exit_code == 0 {
                Ok(())
            } else {
                Err(ffx_error!("zxdb exited with code {}", exit_code).into())
            }
        } else {
            Err(ffx_error!("zxdb terminated by signal").into())
        }
    }
}

struct File {
    filename: String,
    modification_time: u64, // in nanoseconds since epoch
    proxy: fio::FileProxy,
}

// List all remote minidumps remotely, ask the user to make a choice, copy the minidump
// to a temp location and return that path.
async fn choose_and_copy_remote_minidumps(rcs: &RemoteControlProxy) -> Result<TempPath> {
    let mut minidumps = list_minidumps(rcs, "cache").await?;
    minidumps.extend(list_minidumps(rcs, "tmp").await?.into_iter());
    minidumps.sort_by_key(|f| -(f.modification_time as i64));

    if minidumps.is_empty() {
        ffx_bail!("There's no minidump available on the device.")
    }

    println!("Available minidumps are");
    minidumps.iter().enumerate().for_each(|(i, file)| {
        let time = Local.timestamp_nanos(file.modification_time as i64).format("%D %T%.3f");
        println!("{}: {}  {}", i + 1, time, file.filename,);
    });
    print!("Please make a choice (1-{}): ", minidumps.len());
    std::io::stdout().flush().unwrap();

    // Read a number from stdin.
    let choice = std::io::stdin()
        .lock()
        .lines()
        .next()
        .map(|r| r.ok())
        .flatten()
        .map(|s| s.parse::<usize>().ok())
        .flatten()
        .filter(|i| *i >= 1 && *i <= minidumps.len())
        .ok_or(ffx_error!("Invalid input!"))?;

    copy_as_temp_file(&minidumps[choice - 1].proxy).await
}

// List all the "minidump.dmp" files in "/reports" directory with the given capability.
async fn list_minidumps(rcs: &RemoteControlProxy, capability: &str) -> Result<Vec<File>> {
    let (client, server) = fidl::handle::Channel::create();
    connect_to_realm_query(&rcs)
        .await
        .context("Error in connect_to_realm_query")?
        .connect_to_storage_admin("./core", capability, fidl::endpoints::ServerEnd::new(server))
        .await
        .context("FDIL error in get_storage_admin")?
        .map_err(|e| anyhow!("Error in get_storage_admin: {:?}", e))?;

    let storage_admin =
        fidl_fuchsia_sys2::StorageAdminProxy::new(fidl::AsyncChannel::from_channel(client)?);

    let (root_dir, server_end) = fidl::endpoints::create_proxy::<fio::DirectoryMarker>()?;
    storage_admin
        .open_component_storage_by_id(STORAGE_ID, server_end.into_channel().into())
        .await?
        .map_err(|e| anyhow!("Could not open component storage: {:?}", e))?;

    // NOTE: This is the implementation detail of feedback.cm and is subject to change.
    let reports_dir =
        open_directory_no_describe(&root_dir, "reports", fio::OpenFlags::RIGHT_READABLE)
            .context("Could not open the \"reports\" directory")?;
    let reports_dir_ref = &reports_dir;

    futures::future::try_join_all(
        fuchsia_fs::directory::readdir_recursive(reports_dir_ref, None)
            .collect::<Vec<_>>()
            .await
            .into_iter()
            .collect::<Result<Vec<_>, _>>()?
            .into_iter()
            .filter(|entry| {
                entry.kind == fuchsia_fs::directory::DirentKind::File
                    && entry.name.ends_with("/minidump.dmp")
            })
            .map(|entry| async move {
                let proxy = open_file_no_describe(
                    reports_dir_ref,
                    &entry.name,
                    fio::OpenFlags::RIGHT_READABLE,
                )?;
                let (status, fio::NodeAttributes { modification_time, .. }) =
                    proxy.get_attr().await.context("FIDL error in get_attr")?;

                if status != 0 {
                    bail!("Failed to get_attr: {}", Status::from_raw(status));
                }

                Ok(File { filename: entry.name, modification_time, proxy })
            }),
    )
    .await
}

// Copy a remote file to a local temporary path.
async fn copy_as_temp_file(file: &fio::FileProxy) -> Result<TempPath> {
    let mut temp_file = NamedTempFile::new()?;

    loop {
        let bytes = file
            .read(fio::MAX_BUF)
            .await?
            .map_err(|s| anyhow!("Failed to read: {}", Status::from_raw(s)))?;
        if bytes.is_empty() {
            break;
        }
        temp_file.write_all(&bytes)?;
    }

    Ok(temp_file.into_temp_path())
}
