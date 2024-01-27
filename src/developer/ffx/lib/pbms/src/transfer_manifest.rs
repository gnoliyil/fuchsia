// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Download files referenced in a Transfer Manifest JSON file.
//!
//! This builds upon the lower level /src/lib/transfer_manifest lib.

use crate::{
    pbms::{fetch_from_url, GS_SCHEME},
    string_from_url, AuthFlowChoice,
};
use ::gcs::{
    client::{Client, ProgressResult, ProgressState},
    gs_url::split_gs_url,
};
use ::transfer_manifest::TransferManifest;
use anyhow::{Context, Result};
use futures::{StreamExt as _, TryStreamExt as _};
use structured_ui;

/// Download a set of files referenced in the `transfer_manifest_url`.
///
/// Files will be nested under `local_dir` which must exist when this function
/// is called.
pub async fn transfer_download<F, I>(
    transfer_manifest_url: &url::Url,
    local_dir: &std::path::Path,
    auth_flow: &AuthFlowChoice,
    progress: &F,
    ui: &I,
    client: &Client,
) -> Result<()>
where
    F: Fn(Vec<ProgressState<'_>>) -> ProgressResult,
    I: structured_ui::Interface + Sync,
{
    let start = std::time::Instant::now();
    tracing::debug!(
        "transfer_download, transfer_manifest_url {:?}, local_dir {:?}",
        transfer_manifest_url,
        local_dir
    );
    assert!(local_dir.is_dir());

    let tm = string_from_url(&transfer_manifest_url, auth_flow, &|f| progress(vec![f]), ui, client)
        .await
        .with_context(|| format!("string from gcs: {:?}", transfer_manifest_url))?;

    let manifest = serde_json::from_str::<TransferManifest>(&tm)
        .with_context(|| format!("Parsing json {:?}", tm))?;
    match &manifest {
        TransferManifest::V1(v1_data) => transfer_download_v1(
            transfer_manifest_url,
            v1_data,
            local_dir,
            auth_flow,
            progress,
            ui,
            client,
        )
        .await
        .context("transferring from v1 manifest")?,
    }
    tracing::debug!("Total fetch images runtime {} seconds.", start.elapsed().as_secs_f32());
    Ok(())
}

/// Helper for transfer_download specifically for version 1 transfer manifests.
///
/// Files will be nested under `local_dir` which must exist when this function
/// is called.
async fn transfer_download_v1<F, I>(
    transfer_manifest_url: &url::Url,
    transfer_manifest: &transfer_manifest::TransferManifestV1,
    local_dir: &std::path::Path,
    auth_flow: &AuthFlowChoice,
    progress: &F,
    ui: &I,
    client: &Client,
) -> Result<()>
where
    F: Fn(Vec<ProgressState<'_>>) -> ProgressResult,
    I: structured_ui::Interface + Sync,
{
    let base_url = match transfer_manifest_url.scheme() {
        GS_SCHEME => format!(
            "gs://{}",
            split_gs_url(&transfer_manifest_url.as_str())
                .context("splitting transfer_manifest_url")?
                .0
        ),
        _ => transfer_manifest_url[..url::Position::BeforePath].to_string(),
    };
    let mut tasks = Vec::new();
    let transfer_entry_count = transfer_manifest.entries.len() as u64;
    for (i, transfer_entry) in transfer_manifest.entries.iter().enumerate() {
        // Avoid using base_url.join().
        let te_remote_dir = format!("{}/{}", base_url, transfer_entry.remote.as_str());

        let te_local_dir = local_dir.join(&transfer_entry.local);
        let artifact_entry_count = transfer_entry.entries.len() as u64;
        for (k, artifact_entry) in transfer_entry.entries.iter().enumerate() {
            // Avoid using te_remote_dir.join().
            let remote_file =
                url::Url::parse(&format!("{}/{}", te_remote_dir, artifact_entry.name.as_str()))?;

            let local_file = te_local_dir.join(&artifact_entry.name);
            let local_parent = local_file.parent().context("getting local parent")?.to_path_buf();
            async_fs::create_dir_all(&local_parent)
                .await
                .with_context(|| format!("creating local_parent {:?}", local_parent))?;

            tracing::debug!("Transfer {:?} to {:?}", remote_file, local_parent);
            tasks.push(async move {
                fetch_from_url(
                    &remote_file,
                    local_parent,
                    auth_flow,
                    &|_, f| {
                        // The directory progress is replaced because a transfer
                        // manifest refers to specific files to copy (not
                        // directories at time), so the directory progress is
                        // always "1 of 1 files", which is not helpful.
                        let section = ProgressState {
                            name: &transfer_entry.remote.as_str(),
                            at: i as u64 + 1,
                            of: transfer_entry_count,
                            units: "sections",
                        };
                        let directory = ProgressState {
                            name: &remote_file.as_str(),
                            at: k as u64,
                            of: artifact_entry_count,
                            units: "files",
                        };
                        progress(vec![section, directory, f])
                    },
                    ui,
                    &client.clone(),
                )
                .await
            })
        }
    }
    let mut stream = futures::stream::iter(tasks.into_iter())
        .buffer_unordered(std::thread::available_parallelism()?.get());

    while let Some(()) = stream.try_next().await? {}
    Ok(())
}
