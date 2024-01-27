// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::{
    common::{
        cmd::{ManifestParams, OemFile},
        file::*,
    },
    manifest::{from_in_tree, from_path, from_sdk},
    sparse::build_sparse_files,
};
use anyhow::{anyhow, bail, Context, Error, Result};
use async_trait::async_trait;
use chrono::{DateTime, Duration, Utc};
use errors::ffx_bail;
use fidl::{
    endpoints::{create_endpoints, ServerEnd},
    Error as FidlError,
};
use fidl_fuchsia_developer_ffx::{
    FastbootProxy, RebootError, RebootListenerMarker, RebootListenerRequest,
    UploadProgressListenerMarker, UploadProgressListenerRequest,
};
use futures::{prelude::*, try_join};
use sdk::SdkVersion;
use std::{convert::Into, io::Write};
use termion::{color, style};

pub const MISSING_CREDENTIALS: &str =
    "The flash manifest is missing the credential files to unlock this device.\n\
     Please unlock the target and try again.";

pub mod cmd;
pub mod crypto;
pub mod file;
pub mod gcs;

pub trait Partition {
    fn name(&self) -> &str;
    fn file(&self) -> &str;
    fn variable(&self) -> Option<&str>;
    fn variable_value(&self) -> Option<&str>;
}

pub trait Product<P> {
    fn bootloader_partitions(&self) -> &Vec<P>;
    fn partitions(&self) -> &Vec<P>;
    fn oem_files(&self) -> &Vec<OemFile>;
}

#[async_trait(?Send)]
pub trait Flash {
    async fn flash<W, F>(
        &self,
        writer: &mut W,
        file_resolver: &mut F,
        fastboot_proxy: FastbootProxy,
        cmd: ManifestParams,
    ) -> Result<()>
    where
        W: Write,
        F: FileResolver + Sync;
}

#[async_trait(?Send)]
pub trait Unlock {
    async fn unlock<W, F>(
        &self,
        _writer: &mut W,
        _file_resolver: &mut F,
        _fastboot_proxy: FastbootProxy,
    ) -> Result<()>
    where
        W: Write,
        F: FileResolver + Sync,
    {
        ffx_bail!(
            "This manifest does not support unlocking target devices. \n\
        Please update to a newer version of manifest and try again."
        )
    }
}

#[async_trait(?Send)]
pub trait Boot {
    async fn boot<W, F>(
        &self,
        _writer: &mut W,
        _file_resolver: &mut F,
        _slot: String,
        _fastboot_proxy: FastbootProxy,
        _cmd: ManifestParams,
    ) -> Result<()>
    where
        W: Write,
        F: FileResolver + Sync;
}

pub const MISSING_PRODUCT: &str = "Manifest does not contain product";

const LARGE_FILE: &str = "large file, please wait... ";
pub const REVISION_VAR: &str = "hw-revision";
pub const IS_USERSPACE_VAR: &str = "is-userspace";
pub const MAX_DOWNLOAD_SIZE_VAR: &str = "max-download-size";

pub const LOCKED_VAR: &str = "vx-locked";
const LOCK_COMMAND: &str = "vx-lock";

pub const UNLOCK_ERR: &str = "The product requires the target to be unlocked. \
                                     Please unlock target and try again.";

pub fn map_fidl_error(e: FidlError) -> Error {
    tracing::error!("FIDL Communication error: {}", e);
    anyhow!(
        "There was an error communicating with the daemon:\n{}\n\
        Try running `ffx doctor` for further diagnositcs.",
        e
    )
}

pub fn done_time<W: Write>(writer: &mut W, duration: Duration) -> std::io::Result<()> {
    writeln!(
        writer,
        "{}Done{} [{}{:.2}s{}]",
        color::Fg(color::Green),
        style::Reset,
        color::Fg(color::Blue),
        (duration.num_milliseconds() as f32) / (1000 as f32),
        style::Reset
    )?;
    writer.flush()
}

async fn handle_upload_progress_for_upload<W: Write>(
    writer: &mut W,
    prog_server: ServerEnd<UploadProgressListenerMarker>,
    mut on_large: impl FnMut() -> (),
    mut on_finished: impl FnMut(&mut W) -> Result<()>,
) -> Result<Option<DateTime<Utc>>> {
    let mut stream = prog_server.into_stream()?;
    let mut start_time: Option<DateTime<Utc>> = None;
    let mut finish_time: Option<DateTime<Utc>> = None;
    loop {
        match stream.try_next().await? {
            Some(UploadProgressListenerRequest::OnStarted { size, .. }) => {
                start_time.replace(Utc::now());
                tracing::debug!("Upload started: {}", size);
                write!(writer, "Uploading... ")?;
                if size > (1 << 24) {
                    on_large();
                    write!(writer, "{}", LARGE_FILE)?;
                }
                writer.flush()?;
            }
            Some(UploadProgressListenerRequest::OnFinished { .. }) => {
                if let Some(st) = start_time {
                    let d = Utc::now().signed_duration_since(st);
                    tracing::debug!("Upload duration: {:.2}s", (d.num_milliseconds() / 1000));
                    done_time(writer, d)?;
                } else {
                    // Write done without the time .
                    writeln!(writer, "{}Done{}", color::Fg(color::Green), style::Reset)?;
                    writer.flush()?;
                }
                on_finished(writer)?;
                finish_time.replace(Utc::now());
                tracing::debug!("Upload finished");
            }
            Some(UploadProgressListenerRequest::OnError { error, .. }) => {
                tracing::error!("{}", error);
                ffx_bail!("{}", error)
            }
            Some(UploadProgressListenerRequest::OnProgress { bytes_written, .. }) => {
                tracing::debug!("Upload progress: {}", bytes_written);
            }
            None => return Ok(finish_time),
        }
    }
}

async fn handle_upload_progress_for_staging<W: Write>(
    writer: &mut W,
    prog_server: ServerEnd<UploadProgressListenerMarker>,
) -> Result<Option<DateTime<Utc>>> {
    handle_upload_progress_for_upload(writer, prog_server, move || {}, move |_writer| Ok(())).await
}

async fn handle_upload_progress_for_flashing<W: Write>(
    name: &str,
    writer: &mut W,
    prog_server: ServerEnd<UploadProgressListenerMarker>,
) -> Result<Option<DateTime<Utc>>> {
    // Using a boolean results in a warning that the variable is never read.
    let mut is_large: Option<()> = None;
    handle_upload_progress_for_upload(
        writer,
        prog_server,
        move || {
            is_large.replace(());
        },
        move |writer| {
            write!(writer, "Partitioning {}... ", name)?;
            if is_large.is_some() {
                write!(writer, "{}", LARGE_FILE)?;
            }
            writer.flush()?;
            Ok(())
        },
    )
    .await
}

pub async fn stage_file<W: Write, F: FileResolver + Sync>(
    writer: &mut W,
    file_resolver: &mut F,
    resolve: bool,
    file: &str,
    fastboot_proxy: &FastbootProxy,
) -> Result<()> {
    let (prog_client, prog_server) = create_endpoints::<UploadProgressListenerMarker>();
    let file_to_upload = if resolve {
        file_resolver.get_file(writer, file).await.context("reconciling file for upload")?
    } else {
        file.to_string()
    };
    writeln!(writer, "Preparing to stage {}", file_to_upload)?;
    try_join!(
        fastboot_proxy.stage(&file_to_upload, prog_client).map_err(map_fidl_error),
        handle_upload_progress_for_staging(writer, prog_server),
    )
    .and_then(|(stage, _)| {
        stage.map_err(|e| anyhow!("There was an error staging {}: {:?}", file_to_upload, e))
    })
}

#[tracing::instrument(skip(writer))]
async fn do_flash<W: Write>(
    writer: &mut W,
    name: &str,
    fastboot_proxy: &FastbootProxy,
    file_to_upload: &str,
) -> Result<()> {
    let (prog_client, prog_server) = create_endpoints::<UploadProgressListenerMarker>();
    try_join!(
        fastboot_proxy.flash(name, file_to_upload, prog_client).map_err(map_fidl_error),
        handle_upload_progress_for_flashing(name, writer, prog_server),
    )
    .and_then(|(flash, prog)| {
        if let Some(p) = prog {
            let d = Utc::now().signed_duration_since(p);
            tracing::debug!("Partition duration: {:.2}s", (d.num_milliseconds() / 1000));
            done_time(writer, d)?;
        } else {
            // Write a line break otherwise
            writeln!(writer, "{}Done{}", color::Fg(color::Green), style::Reset)?;
            writer.flush()?;
        }
        flash.map_err(|e| {
            anyhow!("There was an error flashing \"{}\" - {}: {:?}", name, file_to_upload, e)
        })
    })
}

#[tracing::instrument(skip(writer))]
async fn flash_partition_sparse<W: Write>(
    writer: &mut W,
    name: &str,
    file_to_upload: &str,
    fastboot_proxy: &FastbootProxy,
    max_download_size: u64,
) -> Result<()> {
    writeln!(writer, "Preparing to flash {} in sparse mode", file_to_upload)?;

    let sparse_files = build_sparse_files(
        writer,
        name,
        file_to_upload,
        std::env::temp_dir().as_path(),
        max_download_size,
    )
    .await?;
    for tmp_file_path in sparse_files {
        let tmp_file_name = tmp_file_path.to_str().unwrap();
        writeln!(writer, "For partition: {}, flashing sparse image file {}", name, tmp_file_name)?;

        do_flash(writer, name, fastboot_proxy, tmp_file_name).await?;
    }

    Ok(())
}

#[tracing::instrument(skip(writer, file_resolver))]
pub async fn flash_partition<W: Write, F: FileResolver + Sync>(
    writer: &mut W,
    file_resolver: &mut F,
    name: &str,
    file: &str,
    fastboot_proxy: &FastbootProxy,
) -> Result<()> {
    let file_to_upload =
        file_resolver.get_file(writer, file).await.context("reconciling file for upload")?;
    writeln!(writer, "Preparing to upload {}", file_to_upload)?;

    // If the given file to flash is bigger than what the device can download
    // at once, we need to make a sparse image out of the given file
    let file_handle = async_fs::File::open(&file_to_upload)
        .await
        .map_err(|e| anyhow!("Got error trying to open file \"{}\": {}", file_to_upload, e))?;
    let file_size = file_handle
        .metadata()
        .await
        .map_err(|e| {
            anyhow!("Got error retrieving metadata for file \"{}\": {}", file_to_upload, e)
        })?
        .len();

    let max_download_size_var = fastboot_proxy
        .get_var(MAX_DOWNLOAD_SIZE_VAR)
        .await
        .map_err(map_fidl_error)?
        .map_err(|e| anyhow!("Communication error with the device: {:?}", e))?;

    tracing::trace!("Got max download size from device: {}", max_download_size_var);
    let trimmed_max_download_size_var = max_download_size_var.trim_start_matches("0x");

    let max_download_size: u64 = u64::from_str_radix(trimmed_max_download_size_var, 16)
        .expect("Fastboot max download size var was not a valid u32");

    tracing::trace!("Device Max Download Size: {}", max_download_size);
    tracing::trace!("File size: {}", file_size);

    if u64::from(max_download_size) < file_size {
        writeln!(
            writer,
            "File size ({}) is bigger than device Max Download Size ({})... \
            Flashing image in Sparse mode",
            file_size, max_download_size
        )?;
        return flash_partition_sparse(
            writer,
            name,
            &file_to_upload,
            fastboot_proxy,
            max_download_size,
        )
        .await;
    }
    do_flash(writer, name, fastboot_proxy, &file_to_upload).await
}

pub async fn verify_hardware(revision: &String, fastboot_proxy: &FastbootProxy) -> Result<()> {
    let rev = fastboot_proxy
        .get_var(REVISION_VAR)
        .await
        .map_err(map_fidl_error)?
        .map_err(|e| anyhow!("Communication error with the device: {:?}", e))?;
    if let Some(r) = rev.split("-").next() {
        if r != *revision && rev != *revision {
            ffx_bail!(
                "Hardware mismatch! Trying to flash images built for {} but have {}",
                revision,
                r
            );
        }
    } else {
        ffx_bail!("Could not verify hardware revision of target device");
    }
    Ok(())
}

pub async fn verify_variable_value(
    var: &str,
    value: &str,
    fastboot_proxy: &FastbootProxy,
) -> Result<bool> {
    fastboot_proxy
        .get_var(var)
        .await
        .map_err(map_fidl_error)?
        .map_err(|e| anyhow!("Communication error with the device: {:?}", e))
        .map(|res| res == value)
}

#[tracing::instrument(skip(writer))]
pub async fn reboot_bootloader<W: Write>(
    writer: &mut W,
    fastboot_proxy: &FastbootProxy,
) -> Result<()> {
    write!(writer, "Rebooting to bootloader... ")?;
    writer.flush()?;
    let (reboot_client, reboot_server) = create_endpoints::<RebootListenerMarker>();
    let mut stream = reboot_server.into_stream()?;
    let start_time = Utc::now();
    try_join!(fastboot_proxy.reboot_bootloader(reboot_client).map_err(map_fidl_error), async move {
        if let Some(RebootListenerRequest::OnReboot { control_handle: _ }) =
            stream.try_next().await?
        {
            Ok(())
        } else {
            bail!("Did not receive reboot signal");
        }
    })
    .and_then(|(reboot, _)| {
        let d = Utc::now().signed_duration_since(start_time);
        tracing::debug!("Reboot duration: {:.2}s", (d.num_milliseconds() / 1000));
        done_time(writer, d)?;
        reboot.or_else(map_reboot_error)
    })
}

const REBOOT_MANUALLY: &str =
    "\nIf the device did not reboot into the Fastboot state, try rebooting \n\
     it manually and re-running the command. Otherwise, try re-running the \n\
     command.";

const TIMED_OUT: &str = "\nTimed out while waiting to rediscover device in Fastboot.";
const SEND_TARGET_REBOOT: &str = "\nFailed while sending the target a reboot signal.";
const SEND_ON_REBOOT: &str = "\nThere was an issue communication with the daemon.";
const ZEDBOOT_COMMUNICATION: &str = "\nFailed to send the Zedboot reboot signal.";
const NO_ZEDBOOT_ADDRESS: &str = "\nUnknown Zedboot address.";
const TARGET_COMMUNICATION: &str = "\nThere was an issue communication with the target";
const FASTBOOT_ERROR: &str = "\nThere was an issue sending the Fastboot reboot command";

fn map_reboot_error(err: RebootError) -> Result<()> {
    match err {
        RebootError::TimedOut => ffx_bail!("{}{}", TIMED_OUT, REBOOT_MANUALLY),
        RebootError::FailedToSendTargetReboot => {
            ffx_bail!("{}{}", SEND_TARGET_REBOOT, REBOOT_MANUALLY)
        }
        RebootError::FailedToSendOnReboot => bail!("{}", SEND_ON_REBOOT),
        RebootError::ZedbootCommunicationError => {
            ffx_bail!("{}{}", ZEDBOOT_COMMUNICATION, REBOOT_MANUALLY)
        }
        RebootError::NoZedbootAddress => bail!("{}", NO_ZEDBOOT_ADDRESS),
        RebootError::TargetCommunication => {
            ffx_bail!("{}{}", TARGET_COMMUNICATION, REBOOT_MANUALLY)
        }
        RebootError::FastbootError => ffx_bail!("{}", FASTBOOT_ERROR),
    }
}

pub async fn prepare<W: Write>(writer: &mut W, fastboot_proxy: &FastbootProxy) -> Result<()> {
    let (reboot_client, reboot_server) = create_endpoints::<RebootListenerMarker>();
    let mut stream = reboot_server.into_stream()?;
    let mut start_time = None;
    writer.flush()?;
    try_join!(fastboot_proxy.prepare(reboot_client).map_err(map_fidl_error), async {
        if let Some(RebootListenerRequest::OnReboot { control_handle: _ }) =
            stream.try_next().await?
        {
            start_time.replace(Utc::now());
            write!(writer, "Rebooting to bootloader... ")?;
            writer.flush()?;
        }
        Ok(())
    })
    .and_then(|(prepare, _)| {
        if let Some(s) = start_time {
            let d = Utc::now().signed_duration_since(s);
            tracing::debug!("Reboot duration: {:.2}s", (d.num_milliseconds() / 1000));
            done_time(writer, d)?;
        }
        prepare.or_else(map_reboot_error)
    })
}

pub async fn stage_oem_files<W: Write, F: FileResolver + Sync>(
    writer: &mut W,
    file_resolver: &mut F,
    resolve: bool,
    oem_files: &Vec<OemFile>,
    fastboot_proxy: &FastbootProxy,
) -> Result<()> {
    for oem_file in oem_files {
        stage_file(writer, file_resolver, resolve, oem_file.file(), &fastboot_proxy).await?;
        writeln!(writer, "Sending command \"{}\"", oem_file.command())?;
        fastboot_proxy.oem(oem_file.command()).await?.map_err(|_| {
            anyhow!("There was an error sending oem command \"{}\"", oem_file.command())
        })?;
    }
    Ok(())
}

#[tracing::instrument(skip(writer, file_resolver, partitions))]
pub async fn flash_partitions<W: Write, F: FileResolver + Sync, P: Partition>(
    writer: &mut W,
    file_resolver: &mut F,
    partitions: &Vec<P>,
    fastboot_proxy: &FastbootProxy,
) -> Result<()> {
    for partition in partitions {
        match (partition.variable(), partition.variable_value()) {
            (Some(var), Some(value)) => {
                if verify_variable_value(var, value, fastboot_proxy).await? {
                    flash_partition(
                        writer,
                        file_resolver,
                        partition.name(),
                        partition.file(),
                        fastboot_proxy,
                    )
                    .await?;
                }
            }
            _ => {
                flash_partition(
                    writer,
                    file_resolver,
                    partition.name(),
                    partition.file(),
                    fastboot_proxy,
                )
                .await?
            }
        }
    }
    Ok(())
}

#[tracing::instrument(skip(writer, file_resolver, product, cmd))]
pub async fn flash<W, F, Part, P>(
    writer: &mut W,
    file_resolver: &mut F,
    product: &P,
    fastboot_proxy: &FastbootProxy,
    cmd: ManifestParams,
) -> Result<()>
where
    W: Write,
    F: FileResolver + Sync,
    Part: Partition,
    P: Product<Part>,
{
    flash_bootloader(writer, file_resolver, product, fastboot_proxy, &cmd).await?;
    flash_product(writer, file_resolver, product, fastboot_proxy, &cmd).await
}

pub async fn is_userspace_fastboot(fastboot_proxy: &FastbootProxy) -> Result<bool> {
    match fastboot_proxy.get_var(IS_USERSPACE_VAR).await.map_err(map_fidl_error)? {
        Ok(rev) => Ok(rev == "yes"),
        _ => Ok(false),
    }
}

#[tracing::instrument(skip(file_resolver, writer, cmd, product))]
pub async fn flash_bootloader<W, F, Part, P>(
    writer: &mut W,
    file_resolver: &mut F,
    product: &P,
    fastboot_proxy: &FastbootProxy,
    cmd: &ManifestParams,
) -> Result<()>
where
    W: Write,
    F: FileResolver + Sync,
    Part: Partition,
    P: Product<Part>,
{
    flash_partitions(writer, file_resolver, product.bootloader_partitions(), fastboot_proxy)
        .await?;
    if product.bootloader_partitions().len() > 0
        && !cmd.no_bootloader_reboot
        && !is_userspace_fastboot(fastboot_proxy).await?
    {
        reboot_bootloader(writer, &fastboot_proxy).await?;
    }
    Ok(())
}

#[tracing::instrument(skip(writer, file_resolver, cmd, product))]
pub async fn flash_product<W, F, Part, P>(
    writer: &mut W,
    file_resolver: &mut F,
    product: &P,
    fastboot_proxy: &FastbootProxy,
    cmd: &ManifestParams,
) -> Result<()>
where
    W: Write,
    F: FileResolver + Sync,
    Part: Partition,
    P: Product<Part>,
{
    flash_partitions(writer, file_resolver, product.partitions(), fastboot_proxy).await?;
    if !cmd.no_bootloader_reboot && is_userspace_fastboot(fastboot_proxy).await? {
        write!(writer, "Rebooting into updated userspace fastboot...\n")?;
        reboot_bootloader(writer, &fastboot_proxy).await?;
    }
    stage_oem_files(writer, file_resolver, false, &cmd.oem_stage, fastboot_proxy).await?;
    stage_oem_files(writer, file_resolver, true, product.oem_files(), fastboot_proxy).await
}

#[tracing::instrument(skip(writer, file_resolver, cmd, product))]
pub async fn flash_and_reboot<W, F, Part, P>(
    writer: &mut W,
    file_resolver: &mut F,
    product: &P,
    fastboot_proxy: &FastbootProxy,
    cmd: ManifestParams,
) -> Result<()>
where
    W: Write,
    F: FileResolver + Sync,
    Part: Partition,
    P: Product<Part>,
{
    flash(writer, file_resolver, product, fastboot_proxy, cmd).await?;
    finish(writer, fastboot_proxy).await
}

pub async fn finish<W: Write>(writer: &mut W, fastboot_proxy: &FastbootProxy) -> Result<()> {
    if fastboot_proxy.erase("misc").await?.is_err() {
        tracing::debug!("Could not erase misc partition");
    }
    fastboot_proxy.set_active("a").await?.map_err(|_| anyhow!("Could not set active slot"))?;
    fastboot_proxy.continue_boot().await?.map_err(|_| anyhow!("Could not reboot device"))?;
    writeln!(writer, "Continuing to boot - this could take awhile")?;
    Ok(())
}

pub async fn is_locked(fastboot_proxy: &FastbootProxy) -> Result<bool> {
    verify_variable_value(LOCKED_VAR, "no", &fastboot_proxy).await.map(|l| !l)
}

pub async fn lock_device(fastboot_proxy: &FastbootProxy) -> Result<()> {
    fastboot_proxy.oem(LOCK_COMMAND).await?.map_err(|_| anyhow!("Could not lock device"))
}

pub async fn from_manifest<W, C>(
    writer: &mut W,
    input: C,
    fastboot_proxy: FastbootProxy,
) -> Result<()>
where
    W: Write,
    C: Into<ManifestParams>,
{
    let cmd: ManifestParams = input.into();
    match &cmd.manifest {
        Some(manifest) => {
            if !manifest.is_file() {
                ffx_bail!("Manifest \"{}\" is not a file.", manifest.display());
            }
            from_path(writer, manifest.to_path_buf(), fastboot_proxy, cmd).await
        }
        None => {
            let sdk = ffx_config::global_env_context()
                .context("loading global environment context")?
                .get_sdk()
                .await?;
            let mut path = sdk.get_path_prefix().to_path_buf();
            writeln!(writer, "No manifest path was given, using SDK from {}.", path.display())?;
            path.push("flash.json"); // Not actually used, placeholder value needed.
            match sdk.get_version() {
                SdkVersion::InTree => from_in_tree(&sdk, writer, path, fastboot_proxy, cmd).await,
                SdkVersion::Version(_) => from_sdk(&sdk, writer, fastboot_proxy, cmd).await,
                _ => ffx_bail!("Unknown SDK type"),
            }
        }
    }
}
