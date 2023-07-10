// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use async_trait::async_trait;
use errors::ffx_bail;
use ffx_bootloader_args::{
    BootCommand, BootloaderCommand,
    SubCommand::{Boot, Info, Lock, Unlock},
    UnlockCommand,
};
use ffx_fastboot::{
    boot::boot,
    common::{file::EmptyResolver, from_manifest, prepare},
    info::info,
    lock::lock,
    unlock::unlock,
};
use fho::{FfxContext, FfxMain, FfxTool, SimpleWriter};
use fidl_fuchsia_developer_ffx::FastbootProxy;
use std::io::{stdin, Write};

const MISSING_ZBI: &str = "Error: vbmeta parameter must be used with zbi parameter";

const WARNING: &str = "WARNING: ALL SETTINGS USER CONTENT WILL BE ERASED!\n\
                        Do you want to continue? [yN]";

#[derive(FfxTool)]
pub struct BootloaderTool {
    #[command]
    cmd: BootloaderCommand,
    fastboot_proxy: FastbootProxy,
}

fho::embedded_plugin!(BootloaderTool);

#[async_trait(?Send)]
impl FfxMain for BootloaderTool {
    type Writer = SimpleWriter;
    async fn main(self, mut writer: Self::Writer) -> fho::Result<()> {
        bootloader_impl(self.fastboot_proxy, self.cmd, &mut writer).await
    }
}

pub async fn bootloader_impl<W: Write>(
    fastboot_proxy: FastbootProxy,
    cmd: BootloaderCommand,
    writer: &mut W,
) -> fho::Result<()> {
    // SubCommands can overwrite the manifest with their own parameters, so check for those
    // conditions before continuing through to check the flash manifest.
    match &cmd.subcommand {
        Info(_) => return info(writer, &fastboot_proxy).await.map_err(fho::Error::from),
        Lock(_) => return lock(writer, &fastboot_proxy).await.map_err(fho::Error::from),
        Unlock(UnlockCommand { cred, force }) => {
            if !force {
                writeln!(writer, "{}", WARNING).bug_context("failed to write")?;
                let answer = blocking::unblock(|| {
                    use std::io::BufRead;
                    let mut line = String::new();
                    let stdin = stdin();
                    let mut locked = stdin.lock();
                    let _ = locked.read_line(&mut line);
                    line
                })
                .await;
                if answer.trim() != "y" {
                    ffx_bail!("User aborted");
                }
            }
            match cred {
                Some(cred_file) => {
                    prepare(writer, &fastboot_proxy).await?;
                    return unlock(
                        writer,
                        &mut EmptyResolver::new()?,
                        &vec![cred_file.to_string()],
                        &fastboot_proxy,
                    )
                    .await
                    .map_err(fho::Error::from);
                }
                _ => {}
            }
        }
        Boot(BootCommand { zbi, vbmeta, .. }) => {
            if vbmeta.is_some() && zbi.is_none() {
                ffx_bail!("{}", MISSING_ZBI)
            }
            match zbi {
                Some(z) => {
                    prepare(writer, &fastboot_proxy).await?;
                    return boot(
                        writer,
                        &mut EmptyResolver::new()?,
                        z.to_owned(),
                        vbmeta.to_owned(),
                        &fastboot_proxy,
                    )
                    .await
                    .map_err(fho::Error::from);
                }
                _ => {}
            }
        }
    }

    from_manifest(writer, cmd, fastboot_proxy).await.map_err(fho::Error::from)
}

////////////////////////////////////////////////////////////////////////////////
// tests

#[cfg(test)]
mod test {
    use super::*;
    use ffx_bootloader_args::LockCommand;
    use ffx_fastboot::{common::LOCKED_VAR, test::setup};
    use tempfile::NamedTempFile;

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_boot_stages_file_and_calls_boot() -> fho::Result<()> {
        let zbi_file = NamedTempFile::new().expect("tmp access failed");
        let zbi_file_name = zbi_file.path().to_string_lossy().to_string();
        let vbmeta_file = NamedTempFile::new().expect("tmp access failed");
        let vbmeta_file_name = vbmeta_file.path().to_string_lossy().to_string();
        let (state, proxy) = setup();
        bootloader_impl(
            proxy,
            BootloaderCommand {
                manifest: None,
                product: "Fuchsia".to_string(),
                product_bundle: None,
                skip_verify: false,
                subcommand: Boot(BootCommand {
                    zbi: Some(zbi_file_name),
                    vbmeta: Some(vbmeta_file_name),
                    slot: "a".to_string(),
                }),
            },
            &mut std::io::stdout(),
        )
        .await?;
        let state = state.lock().unwrap();
        assert_eq!(1, state.staged_files.len());
        assert_eq!(1, state.boots);
        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_boot_stages_file_and_calls_boot_with_just_zbi() -> fho::Result<()> {
        let zbi_file = NamedTempFile::new().expect("tmp access failed");
        let zbi_file_name = zbi_file.path().to_string_lossy().to_string();
        let (state, proxy) = setup();
        bootloader_impl(
            proxy,
            BootloaderCommand {
                manifest: None,
                product: "Fuchsia".to_string(),
                product_bundle: None,
                skip_verify: false,
                subcommand: Boot(BootCommand {
                    zbi: Some(zbi_file_name),
                    vbmeta: None,
                    slot: "a".to_string(),
                }),
            },
            &mut std::io::stdout(),
        )
        .await?;
        let state = state.lock().unwrap();
        assert_eq!(1, state.staged_files.len());
        assert_eq!(1, state.boots);
        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_boot_fails_with_just_vbmeta() {
        let vbmeta_file = NamedTempFile::new().expect("tmp access failed");
        let vbmeta_file_name = vbmeta_file.path().to_string_lossy().to_string();
        let (_, proxy) = setup();
        assert!(bootloader_impl(
            proxy,
            BootloaderCommand {
                manifest: None,
                product: "Fuchsia".to_string(),
                product_bundle: None,
                skip_verify: false,
                subcommand: Boot(BootCommand {
                    zbi: None,
                    vbmeta: Some(vbmeta_file_name),
                    slot: "a".to_string(),
                }),
            },
            &mut std::io::stdout(),
        )
        .await
        .is_err());
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_lock_calls_oem_command() -> fho::Result<()> {
        let (state, proxy) = setup();
        {
            let mut state = state.lock().unwrap();
            // is_locked
            state.set_var(LOCKED_VAR.to_string(), "no".to_string());
            state.set_var("vx-unlockable".to_string(), "no".to_string());
        }
        bootloader_impl(
            proxy,
            BootloaderCommand {
                manifest: None,
                product: "Fuchsia".to_string(),
                product_bundle: None,
                skip_verify: false,
                subcommand: Lock(LockCommand {}),
            },
            &mut std::io::stdout(),
        )
        .await?;
        let state = state.lock().unwrap();
        assert_eq!(1, state.oem_commands.len());
        assert_eq!("vx-lock".to_string(), state.oem_commands[0]);
        Ok(())
    }
}
