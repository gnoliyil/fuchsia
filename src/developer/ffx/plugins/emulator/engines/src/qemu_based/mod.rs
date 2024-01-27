// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! The qemu_base module encapsulates traits and functions specific
//! for engines using QEMU as the emulator platform.

use crate::arg_templates::process_flag_template;
use crate::finalize_port_mapping;
use crate::qemu_based::comms::{spawn_pipe_thread, QemuSocket};
use crate::serialization::SerializingEngine;
use crate::show_output;
use anyhow::{anyhow, bail, Context, Result};
use async_trait::async_trait;
use cfg_if::cfg_if;
use errors::ffx_bail;
use ffx_config::SshKeyFiles;
use ffx_emulator_common::{
    config,
    config::EMU_START_TIMEOUT,
    dump_log_to_out, host_is_mac, process,
    target::{add_target, is_active, remove_target, TargetAddress},
    tuntap::{tap_ready, TAP_INTERFACE_NAME},
};
use ffx_emulator_config::{
    AccelerationMode, ConsoleType, EmulatorConfiguration, EmulatorEngine, EngineConsoleType,
    EngineState, GuestConfig, HostConfig, NetworkingMode, ShowDetail,
};
use fidl_fuchsia_developer_ffx as ffx;
use serde::Serialize;
use shared_child::SharedChild;
use std::env;
use std::fs::{self, File};
use std::io::{stderr, Write};
use std::net::{IpAddr, Ipv4Addr, Shutdown};
use std::ops::Sub;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::str;
use std::sync::{mpsc::channel, Arc};
use std::time::Duration;

#[cfg(test)]
use mockall::automock;

#[cfg_attr(test, automock)]
#[allow(dead_code)]
mod modules {
    use super::*;

    pub(crate) async fn get_host_tool(name: &str) -> Result<PathBuf> {
        let sdk = ffx_config::global_env_context()
            .context("loading global environment context")?
            .get_sdk()
            .await?;

        // Attempts to get a host tool from the SDK manifest. If it fails, falls
        // back to attempting to derive the path to the host tool binary by simply checking
        // for its existence in `ffx`'s directory.
        // TODO(fxb/99321): When issues around including aemu in the sdk are resolved, this
        // hack can be removed.
        match sdk.get_host_tool(name) {
            Ok(path) => Ok(path),
            Err(error) => {
                tracing::warn!(
                    "failed to get host tool {} from manifest. Trying local SDK dir: {}",
                    name,
                    error
                );
                let mut ffx_path = std::env::current_exe()
                    .context(format!("getting current ffx exe path for host tool {}", name))?;
                ffx_path = std::fs::canonicalize(ffx_path.clone())
                    .context(format!("canonicalizing ffx path {:?}", ffx_path))?;

                let tool_path = ffx_path
                    .parent()
                    .context(format!("ffx path missing parent {:?}", ffx_path))?
                    .join(name);

                if tool_path.exists() {
                    Ok(tool_path)
                } else {
                    bail!("Host tool '{}' not found after checking in `ffx` directory.", name);
                }
            }
        }
    }
}

cfg_if! {
    if #[cfg(test)] {
        pub(crate) use self::mock_modules::get_host_tool;
    } else {
        pub(crate) use self::modules::get_host_tool;
    }
}

pub(crate) mod comms;
pub(crate) mod femu;
pub(crate) mod qemu;

const COMMAND_CONSOLE: &str = "./monitor";
const MACHINE_CONSOLE: &str = "./qmp";
const SERIAL_CONSOLE: &str = "./serial";

/// MDNSInfo is the configuration data used by Fuchsia's mdns service.
/// When using user mode networking, an instance of this configuration
/// is added to the boot data so that the local address and port mappings
/// are shared via mdns.
#[derive(Serialize, Debug)]
pub(crate) struct MDNSInfo {
    publications: Vec<MDNSServiceInfo>,
}

#[derive(Serialize, Debug)]
pub(crate) struct MDNSServiceInfo {
    media: String,
    perform_probe: bool,
    port: u16,
    service: String,
    text: Vec<String>,
}

impl MDNSInfo {
    fn new(host: &HostConfig, local_addr: &IpAddr) -> Self {
        let mut info = MDNSInfo {
            publications: vec![MDNSServiceInfo {
                media: "wired".to_string(),
                perform_probe: false,
                port: 22,
                service: "_fuchsia._udp.".to_string(),
                text: vec![format!("host:{}", local_addr)],
            }],
        };

        for (name, mapping) in &host.port_map {
            if let Some(port) = mapping.host {
                info.publications[0].text.push(format!("{}:{}", name, port));
                if name == "ssh" {
                    info.publications[0].port = port;
                }
            }
        }

        info
    }
}

/// QemuBasedEngine collects the interface for
/// emulator engine implementations that use
/// QEMU as the emulator.
/// This allows the implementation to be shared
/// across multiple engine types.
#[async_trait]
pub(crate) trait QemuBasedEngine: EmulatorEngine + SerializingEngine {
    /// Checks that the required files are present
    fn check_required_files(&self, guest: &GuestConfig) -> Result<()> {
        let kernel_path = &guest.kernel_image;
        let zbi_path = &guest.zbi_image;
        let fvm_path = &guest.fvm_image;

        if !kernel_path.exists() {
            bail!("kernel file {:?} does not exist.", kernel_path);
        }
        if !zbi_path.exists() {
            bail!("zbi file {:?} does not exist.", zbi_path);
        }
        if let Some(file_path) = &fvm_path {
            if !file_path.exists() {
                bail!("fvm file {:?} does not exist.", file_path);
            }
        }
        Ok(())
    }

    /// Stages the source image files in an instance specific directory.
    /// Also resizes the fvms to the desired size and adds the authorized
    /// keys to the zbi.
    /// Returns an updated GuestConfig instance with the file paths set to
    /// the instance paths.
    async fn stage_image_files(
        instance_name: &str,
        emu_config: &EmulatorConfiguration,
        mdns_info: &Option<MDNSInfo>,
        reuse: bool,
    ) -> Result<GuestConfig> {
        let mut updated_guest = emu_config.guest.clone();

        // Create the data directory if needed.
        let mut instance_root: PathBuf =
            ffx_config::query(config::EMU_INSTANCE_ROOT_DIR).get_file().await?;
        instance_root.push(instance_name);
        fs::create_dir_all(&instance_root)?;

        let kernel_name = emu_config.guest.kernel_image.file_name().ok_or_else(|| {
            anyhow!("cannot read kernel file name '{:?}'", emu_config.guest.kernel_image)
        });
        let kernel_path = instance_root.join(kernel_name?);
        if kernel_path.exists() && reuse {
            tracing::debug!("Using existing file for {:?}", kernel_path.file_name().unwrap());
        } else {
            fs::copy(&emu_config.guest.kernel_image, &kernel_path)
                .context("cannot stage kernel file")?;
        }

        let zbi_path = instance_root.join(
            emu_config
                .guest
                .zbi_image
                .file_name()
                .ok_or_else(|| anyhow!("cannot read zbi file name"))?,
        );

        if zbi_path.exists() && reuse {
            tracing::debug!("Using existing file for {:?}", zbi_path.file_name().unwrap());
            // TODO(fxbug.dev/112577): Make a decision to reuse zbi with no modifications or not.
            // There is the potential that the ssh keys have changed, or the ip address
            // of the host interface has changed, which will cause the connection
            // to the emulator instance to fail.
        } else {
            // Add the authorized public keys to the zbi image to enable SSH access to
            // the guest.
            Self::embed_boot_data(&emu_config.guest.zbi_image, &zbi_path, mdns_info)
                .await
                .context("cannot embed boot data")?;
        }

        let fvm_path = match &emu_config.guest.fvm_image {
            Some(src_fvm) => {
                let fvm_path = instance_root
                    .join(src_fvm.file_name().ok_or_else(|| anyhow!("cannot read fvm file name"))?);
                if fvm_path.exists() && reuse {
                    tracing::debug!("Using existing file for {:?}", fvm_path.file_name().unwrap());
                } else {
                    fs::copy(src_fvm, &fvm_path).context("cannot stage fvm file")?;

                    // Resize the fvm image if needed.
                    let image_size = format!(
                        "{}{}",
                        emu_config.device.storage.quantity,
                        emu_config.device.storage.units.abbreviate()
                    );
                    let fvm_tool = get_host_tool(config::FVM_HOST_TOOL)
                        .await
                        .context("cannot locate fvm tool")?;
                    let resize_result = Command::new(fvm_tool)
                        .arg(&fvm_path)
                        .arg("extend")
                        .arg("--length")
                        .arg(&image_size)
                        .output()?;

                    if !resize_result.status.success() {
                        bail!("Error resizing fvm: {}", str::from_utf8(&resize_result.stderr)?);
                    }
                }
                Some(fvm_path)
            }
            None => None,
        };

        updated_guest.kernel_image = kernel_path;
        updated_guest.zbi_image = zbi_path;
        updated_guest.fvm_image = fvm_path;
        Ok(updated_guest)
    }

    /// embed_boot_data adds authorized_keys for ssh access to the zbi boot image file.
    /// If mdns_info is Some(), it is also added. This mdns configuration is
    /// read by Fuchsia mdns service and used instead of the default configuration.
    async fn embed_boot_data(
        src: &PathBuf,
        dest: &PathBuf,
        mdns_info: &Option<MDNSInfo>,
    ) -> Result<()> {
        let zbi_tool = get_host_tool(config::ZBI_HOST_TOOL).await.context("ZBI tool is missing")?;
        let ssh_keys = SshKeyFiles::load().await.context("finding ssh authorized_keys file.")?;
        ssh_keys.create_keys_if_needed().context("create ssh keys if needed")?;
        let auth_keys = ssh_keys.authorized_keys.display().to_string();
        if !ssh_keys.authorized_keys.exists() {
            bail!("No authorized_keys found to configure emulator. {} does not exist.", auth_keys);
        }
        if src == dest {
            return Err(anyhow!("source and dest zbi paths cannot be the same."));
        }

        let replace_str = format!("data/ssh/authorized_keys={}", auth_keys);

        let mut zbi_command = Command::new(zbi_tool);
        zbi_command.arg("-o").arg(dest).arg("--replace").arg(src).arg("-e").arg(replace_str);

        if let Some(info) = mdns_info {
            // Save the mdns info to a file.
            let parent = dest.parent().expect("parent dir of zbi should exist.");
            let mdns_config = parent.join("fuchsia_udp.config");
            let mut f = File::create(&mdns_config).context("getting zbi parent directory")?;
            f.write_all(serde_json::to_string(info)?.as_bytes())?;

            // Add it to the zbi command line.
            let emu_svc_str = format!("data/mdns/emu.config={}", mdns_config.display());

            zbi_command.arg("-e").arg(emu_svc_str);
        }

        // added last.
        zbi_command.arg("--type=entropy:64").arg("/dev/urandom");

        let zbi_command_output = zbi_command.output()?;

        if !zbi_command_output.status.success() {
            bail!("Error embedding boot data: {}", str::from_utf8(&zbi_command_output.stderr)?);
        }
        Ok(())
    }

    fn validate_network_flags(&self, emu_config: &EmulatorConfiguration) -> Result<()> {
        match emu_config.host.networking {
            NetworkingMode::None => {
                // Check for console/monitor.
                if emu_config.runtime.console == ConsoleType::None {
                    bail!(
                        "Running without networking enabled and no interactive console;\n\
                        there will be no way to communicate with this emulator.\n\
                        Restart with --console/--monitor or with networking enabled to proceed."
                    );
                }
            }
            NetworkingMode::Auto => {
                // Shouldn't be possible to land here.
                bail!("Networking mode is unresolved after configuration.");
            }
            NetworkingMode::Tap => {
                // Officially, MacOS tun/tap is unsupported. tap_ready() uses the "ip" command to
                // retrieve details about the target interface, but "ip" is not installed on macs
                // by default. That means, if tap_ready() is called on a MacOS host, it returns a
                // Result::Error, which would cancel emulation. However, if an end-user sets up
                // tun/tap on a MacOS host we don't want to block that, so we check the OS here
                // and make it a warning to run on MacOS instead.
                if host_is_mac() {
                    eprintln!(
                        "Tun/Tap networking mode is not currently supported on MacOS. \
                        You may experience errors with your current configuration."
                    );
                } else {
                    // tap_ready() has some good error reporting, so just return the Result.
                    return tap_ready();
                }
            }
            NetworkingMode::User => (),
        }
        Ok(())
    }

    async fn stage(&mut self) -> Result<()> {
        let mut emu_config = self.emu_config_mut();
        let name = emu_config.runtime.name.clone();
        let reuse = emu_config.runtime.reuse;

        // Configure any port mappings before staging files so the
        // port map can be shared with the zbi file.
        let mut mdns_service_info: Option<MDNSInfo> = None;
        if emu_config.host.networking == NetworkingMode::User {
            finalize_port_mapping(emu_config).context("Problem with port mapping")?;

            // For user mode networking always use 127.0.0.1. This avoids any firewall
            // configurations that could interfere and use IPv4 since there is issues with
            // QEMU and IPv6.
            let local_v4_interface = IpAddr::V4(Ipv4Addr::new(127, 0, 0, 1));
            mdns_service_info = Some(MDNSInfo::new(&emu_config.host, &local_v4_interface));
        }

        emu_config.guest = Self::stage_image_files(&name, emu_config, &mdns_service_info, reuse)
            .await
            .context("could not stage image files")?;

        // This is done to avoid running emu in the same directory as the kernel or other files
        // that are used by qemu. If the multiboot.bin file is in the current directory, it does
        // not start correctly. This probably could be temporary until we know the images loaded
        // do not have files directly in $sdk_root.
        env::set_current_dir(emu_config.runtime.instance_directory.parent().unwrap())
            .context("problem changing directory to instance dir")?;

        emu_config.flags = process_flag_template(emu_config)
            .context("Failed to process the flags template file.")?;

        Ok(())
    }

    async fn run(
        &mut self,
        mut emulator_cmd: Command,
        proxy: &ffx::TargetCollectionProxy,
    ) -> Result<i32> {
        if self.emu_config().runtime.console == ConsoleType::None {
            let stdout = File::create(&self.emu_config().host.log)
                .context(format!("Couldn't open log file {:?}", &self.emu_config().host.log))?;
            let stderr = stdout
                .try_clone()
                .context("Failed trying to clone stdout for the emulator process.")?;
            emulator_cmd.stdout(stdout).stderr(stderr);
            println!("Logging to {:?}", &self.emu_config().host.log);
        }

        // If using TAP, check for an upscript to run.
        if let Some(script) = match &self.emu_config().host.networking {
            NetworkingMode::Tap => &self.emu_config().runtime.upscript,
            _ => &None,
        } {
            let status = Command::new(script)
                .arg(TAP_INTERFACE_NAME)
                .status()
                .context(format!("Problem running upscript '{}'", &script.display()))?;
            if !status.success() {
                return Err(anyhow!(
                    "Upscript {} returned non-zero exit code {}",
                    script.display(),
                    status.code().map_or("None".to_string(), |v| format!("{}", v))
                ));
            }
        }

        let shared_process = SharedChild::spawn(&mut emulator_cmd)?;
        let child_arc = Arc::new(shared_process);

        self.set_pid(child_arc.id());
        self.set_engine_state(EngineState::Running);

        self.save_to_disk().context("Failed to write the emulation configuration file to disk.")?;

        let ssh = self.emu_config().host.port_map.get("ssh");
        let ssh_port = if let Some(ssh) = ssh { ssh.host } else { None };
        if self.emu_config().host.networking == NetworkingMode::User {
            // We only need to do this if we're running in user net mode.
            let timeout = self.emu_config().runtime.startup_timeout;
            if let Some(ssh_port) = ssh_port {
                let local_ip = TargetAddress::Loopback;

                // TODO(fxbug.dev/114597): Remove manually added target when obsolete.
                add_target(proxy, local_ip, ssh_port, timeout)
                    .await
                    .context("Failed to add the emulator to the ffx target collection.")?;
            }
        }

        if self.emu_config().runtime.debugger {
            println!("The emulator will wait for a debugger to attach before starting up.");
            println!("Attach to process {} to continue launching the emulator.", self.get_pid());
        }

        if self.emu_config().runtime.console == ConsoleType::Monitor
            || self.emu_config().runtime.console == ConsoleType::Console
        {
            // When running with '--monitor' or '--console' mode, the user is directly interacting
            // with the emulator console, or the guest console. Therefore wait until the
            // execution of QEMU or AEMU terminates.
            match fuchsia_async::unblock(move || process::monitored_child_process(&child_arc)).await
            {
                Ok(_) => {
                    return Ok(0);
                }
                Err(e) => {
                    let name = &self.emu_config().runtime.name;
                    if let Err(e) = remove_target(proxy, name).await {
                        // Even if we can't remove it, still continue shutting down.
                        tracing::warn!("Couldn't remove target from ffx during shutdown: {:?}", e);
                    }
                    if let Some(stop_error) = self.stop_emulator().err() {
                        tracing::debug!(
                            "Error encountered in stop when handling failed launch: {:?}",
                            stop_error
                        );
                    }
                    ffx_bail!("Emulator launcher did not terminate properly, error: {}", e)
                }
            }
        } else if !self.emu_config().runtime.startup_timeout.is_zero() {
            // Wait until the emulator is considered "active" before returning to the user.
            let mut time_left = self.emu_config().runtime.startup_timeout;
            print!("Waiting for Fuchsia to start (up to {} seconds).", &time_left.as_secs());
            tracing::debug!(
                "Waiting for Fuchsia to start (up to {} seconds)...",
                &time_left.as_secs()
            );
            let name = self.emu_config().runtime.name.clone();
            while !time_left.is_zero() {
                if is_active(proxy, &name).await {
                    println!("\nEmulator is ready.");
                    tracing::debug!("Emulator is ready.");
                    break;
                } else {
                    // Output a little status indicator to show we haven't gotten stuck.
                    // Note that we discard the result on the flush call; it's not important enough
                    // that we flushed the output stream to derail the launch.
                    print!(".");
                    std::io::stdout().flush().ok();

                    // Perform a check to make sure the process is still alive, otherwise report
                    // failure to launch.
                    if !self.is_running() {
                        tracing::error!(
                            "Emulator process failed to launch, but we don't know the cause. \
                            Check the emulator log, or look for a crash log."
                        );
                        eprintln!(
                            "\nEmulator process failed to launch, but we don't know the cause. \
                            Printing the contents of the emulator log...\n"
                        );
                        match dump_log_to_out(&self.emu_config().host.log, &mut stderr()) {
                            Ok(_) => (),
                            Err(e) => eprintln!("Couldn't print the log: {:?}", e),
                        };
                        if self.emu_config().host.networking == NetworkingMode::User {
                            // We only need to do this if we're running in user net mode.
                            if let Some(ssh_port) = ssh_port {
                                // TODO(fxbug.dev/114597): Remove manually added target when obsolete.
                                if let Err(e) =
                                    remove_target(proxy, &format!("127.0.0.1:{}", ssh_port)).await
                                {
                                    // A failure here probably means it was never added.
                                    // Just log the error and quit.
                                    tracing::warn!(
                                        "Couldn't remove target from ffx during shutdown: {:?}",
                                        e
                                    );
                                }
                            }
                        }

                        self.set_engine_state(EngineState::Staged);
                        self.save_to_disk()
                            .context("Failed to write the emulation configuration file to disk.")?;

                        return Ok(1);
                    }

                    time_left = time_left.sub(Duration::from_secs(1));
                    if time_left.is_zero() {
                        eprintln!();
                        eprintln!(
                            "After {} seconds, the emulator has not responded to network queries.",
                            self.emu_config().runtime.startup_timeout.as_secs()
                        );
                        if self.is_running() {
                            eprintln!(
                                "The emulator process is still running (pid {}).",
                                self.get_pid()
                            );
                            eprintln!(
                                "The emulator is configured to {} network access.",
                                match self.emu_config().host.networking {
                                    NetworkingMode::Tap => "use tun/tap-based",
                                    NetworkingMode::User => "use user-mode/port-mapped",
                                    NetworkingMode::None => "disable all",
                                    NetworkingMode::Auto => bail!(
                                        "Auto Networking mode should not be possible after staging \
                                        is complete. Configuration is corrupt; bailing out."
                                    ),
                                }
                            );
                            eprintln!(
                                "Hardware acceleration is {}.",
                                if self.emu_config().host.acceleration == AccelerationMode::Hyper {
                                    "enabled"
                                } else {
                                    "disabled, which significantly slows down the emulator"
                                }
                            );
                            eprintln!(
                                "You can execute `ffx target list` to keep monitoring the device, \
                                or `ffx emu stop` to terminate it."
                            );
                            eprintln!(
                                "You can also change the timeout if you keep encountering this \
                                message by executing `ffx config set {} <seconds>`.",
                                EMU_START_TIMEOUT
                            );
                        } else {
                            eprintln!();
                            eprintln!(
                                "Emulator process failed to launch, but we don't know the cause. \
                                Printing the contents of the emulator log...\n"
                            );
                            match dump_log_to_out(
                                &self.emu_config().host.log,
                                &mut std::io::stderr(),
                            ) {
                                Ok(_) => (),
                                Err(e) => eprintln!("Couldn't print the log: {:?}", e),
                            };
                        }

                        tracing::warn!(
                            "Emulator did not respond to a health check before timing out."
                        );
                    }
                }
            }
        }
        Ok(0)
    }

    fn show(&self, details: Vec<ShowDetail>) {
        if details.contains(&ShowDetail::Raw) {
            if self.emu_config().runtime.config_override {
                println!(
                    "Configuration was provided manually to the start command using the --config\n\
                    flag. Details are likely inconsistent between the EmulatorConfiguration and\n\
                    the FlagData used to invoke the emulator.\n"
                );
            }
            println!("{:#?}", self.emu_config());
        } else {
            for segment in details {
                match segment {
                    ShowDetail::Cmd => println!("Command line:  {:#?}", self.build_emulator_cmd()),
                    ShowDetail::Config => show_output::config(self.emu_config()),
                    ShowDetail::Device => show_output::device(self.emu_config()),
                    ShowDetail::Net => show_output::net(self.emu_config()),
                    ShowDetail::Raw | ShowDetail::All =>
                        /* already handled, just needed for completeness */
                        {}
                }
                println!();
            }
        }
    }

    fn stop_emulator(&mut self) -> Result<()> {
        if self.is_running() {
            println!("Terminating running instance {:?}", self.get_pid());
            if let Some(terminate_error) = process::terminate(self.get_pid()).err() {
                tracing::warn!("Error encountered terminating process: {:?}", terminate_error);
            }
        }
        self.set_engine_state(EngineState::Staged);
        self.save_to_disk()
    }

    /// Access to the engine's pid field.
    fn set_pid(&mut self, pid: u32);
    fn get_pid(&self) -> u32;

    /// Access to the engine's engine_state field.
    fn set_engine_state(&mut self, state: EngineState);
    fn get_engine_state(&self) -> EngineState;

    /// Attach to emulator's console socket.
    fn attach_to(&self, path: &Path, console: EngineConsoleType) -> Result<()> {
        let console_path = self.get_path_for_console_type(path, console);
        let mut socket = QemuSocket::new(&console_path);
        socket.connect().context("Connecting to console.")?;
        let stream = socket.stream().ok_or_else(|| anyhow!("No socket connected."))?;
        let (tx, rx) = channel();

        let _t1 = spawn_pipe_thread(std::io::stdin(), stream.try_clone()?, tx.clone());
        let _t2 = spawn_pipe_thread(stream.try_clone()?, std::io::stdout(), tx);

        // Now that the threads are reading and writing, we wait for one to send back an error.
        let error = rx.recv()?;
        eprintln!("{:?}", error);
        stream.shutdown(Shutdown::Both).context("Shutting down stream.")?;
        Ok(())
    }

    fn get_path_for_console_type(&self, path: &Path, console: EngineConsoleType) -> PathBuf {
        path.join(match console {
            EngineConsoleType::Command => COMMAND_CONSOLE,
            EngineConsoleType::Machine => MACHINE_CONSOLE,
            EngineConsoleType::Serial => SERIAL_CONSOLE,
            EngineConsoleType::None => panic!("No path exists for EngineConsoleType::None"),
        })
    }
}

#[cfg(test)]
mod tests {
    use std::io::Read;

    use super::*;
    use async_trait::async_trait;
    use ffx_config::{query, ConfigLevel};
    use ffx_emulator_config::EngineType;
    use serde::Serialize;
    use serde_json::json;
    use tempfile::{tempdir, TempDir};

    #[derive(Default, Serialize)]
    struct TestEngine {}
    impl QemuBasedEngine for TestEngine {
        fn set_pid(&mut self, _pid: u32) {}
        fn get_pid(&self) -> u32 {
            todo!()
        }
        fn set_engine_state(&mut self, _state: EngineState) {}
        fn get_engine_state(&self) -> EngineState {
            todo!()
        }
    }
    #[async_trait]
    impl EmulatorEngine for TestEngine {
        fn engine_state(&self) -> EngineState {
            EngineState::default()
        }
        fn engine_type(&self) -> EngineType {
            EngineType::default()
        }
        fn is_running(&mut self) -> bool {
            false
        }
    }
    impl SerializingEngine for TestEngine {}

    const ORIGINAL: &str = "THIS_STRING";
    const UPDATED: &str = "THAT_VALUE*";

    // Note that the caller MUST initialize the ffx_config environment before calling this function
    // since we override config values as part of the test. This looks like:
    //     let _env = ffx_config::test_init().await?;
    // The returned structure must remain in scope for the duration of the test to function
    // properly.
    async fn setup(guest: &mut GuestConfig, temp: &TempDir) -> Result<PathBuf> {
        let root = temp.path();

        let kernel_path = root.join("kernel");
        let zbi_path = root.join("zbi");
        let fvm_path = root.join("fvm");

        let _ = fs::File::options()
            .write(true)
            .create(true)
            .open(&kernel_path)
            .context("cannot create test kernel file")?;
        let _ = fs::File::options()
            .write(true)
            .create(true)
            .open(&zbi_path)
            .context("cannot create test zbi file")?;
        let _ = fs::File::options()
            .write(true)
            .create(true)
            .open(&fvm_path)
            .context("cannot create test fvm file")?;

        query(config::EMU_INSTANCE_ROOT_DIR)
            .level(Some(ConfigLevel::User))
            .set(json!(root.display().to_string()))
            .await?;

        guest.kernel_image = kernel_path;
        guest.zbi_image = zbi_path;
        guest.fvm_image = Some(fvm_path);

        // Set the paths to use for the SSH keys
        query("ssh.pub")
            .level(Some(ConfigLevel::User))
            .set(json!([root.join("test_authorized_keys")]))
            .await?;
        query("ssh.priv")
            .level(Some(ConfigLevel::User))
            .set(json!([root.join("test_ed25519_key")]))
            .await?;

        Ok(PathBuf::from(root))
    }

    fn write_to(path: &PathBuf, value: &str) -> Result<()> {
        println!("Writing {} to {}", value, path.display());
        let mut file = File::options()
            .write(true)
            .open(path)
            .context(format!("cannot open existing file for write: {}", path.display()))?;
        File::write(&mut file, value.as_bytes())
            .context(format!("cannot write buffer to file: {}", path.display()))?;

        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    #[serial_test::serial]
    async fn test_staging_no_reuse() -> Result<()> {
        let _env = ffx_config::test_init().await?;
        let temp = tempdir().context("cannot get tempdir")?;
        let instance_name = "test-instance";
        let mut emu_config = EmulatorConfiguration::default();
        let root = setup(&mut emu_config.guest, &temp).await?;

        let ctx = mock_modules::get_host_tool_context();
        ctx.expect().returning(|_| Ok(PathBuf::from("echo")));

        write_to(&emu_config.guest.kernel_image, ORIGINAL)
            .context("cannot write original value to kernel file")?;
        write_to(emu_config.guest.fvm_image.as_ref().unwrap(), ORIGINAL)
            .context("cannot write original value to fvm file")?;

        let updated = <TestEngine as QemuBasedEngine>::stage_image_files(
            instance_name,
            &emu_config,
            &None,
            false,
        )
        .await;

        assert!(updated.is_ok(), "expected OK got {:?}", updated.unwrap_err());

        let actual = updated.context("cannot get updated guest config")?;
        let expected = GuestConfig {
            kernel_image: root.join(instance_name).join("kernel"),
            zbi_image: root.join(instance_name).join("zbi"),
            fvm_image: Some(root.join(instance_name).join("fvm")),
        };
        assert_eq!(actual, expected);

        // Test no reuse when old files exist. The original files should be overwritten.
        write_to(&emu_config.guest.kernel_image, UPDATED)
            .context("cannot write updated value to kernel file")?;
        write_to(emu_config.guest.fvm_image.as_ref().unwrap(), UPDATED)
            .context("cannot write updated value to fvm file")?;

        let updated = <TestEngine as QemuBasedEngine>::stage_image_files(
            instance_name,
            &emu_config,
            &None,
            false,
        )
        .await;

        assert!(updated.is_ok(), "expected OK got {:?}", updated.unwrap_err());

        let actual = updated.context("cannot get updated guest config, reuse")?;
        let expected = GuestConfig {
            kernel_image: root.join(instance_name).join("kernel"),
            zbi_image: root.join(instance_name).join("zbi"),
            fvm_image: Some(root.join(instance_name).join("fvm")),
        };
        assert_eq!(actual, expected);

        println!("Reading contents from {}", actual.kernel_image.display());
        println!("Reading contents from {}", actual.fvm_image.as_ref().unwrap().display());
        let mut kernel = File::open(&actual.kernel_image)
            .context("cannot open overwritten kernel file for read")?;
        let mut fvm = File::open(&actual.fvm_image.unwrap())
            .context("cannot open overwritten fvm file for read")?;

        let mut kernel_contents = String::new();
        let mut fvm_contents = String::new();

        kernel
            .read_to_string(&mut kernel_contents)
            .context("cannot read contents of reused kernel file")?;
        fvm.read_to_string(&mut fvm_contents).context("cannot read contents of reused fvm file")?;

        assert_eq!(kernel_contents, UPDATED);
        assert_eq!(fvm_contents, UPDATED);

        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    #[serial_test::serial]
    async fn test_staging_with_reuse() -> Result<()> {
        let _env = ffx_config::test_init().await?;
        let temp = tempdir().context("cannot get tempdir")?;
        let instance_name = "test-instance";
        let mut emu_config = EmulatorConfiguration::default();

        let root = setup(&mut emu_config.guest, &temp).await?;

        let ctx = mock_modules::get_host_tool_context();
        ctx.expect().returning(|_| Ok(PathBuf::from("echo")));

        // This checks if --reuse is true, but the directory isn't there to reuse; should succeed.
        write_to(&emu_config.guest.kernel_image, ORIGINAL)
            .context("cannot write original value to kernel file")?;
        write_to(emu_config.guest.fvm_image.as_ref().unwrap(), ORIGINAL)
            .context("cannot write original value to fvm file")?;

        let updated: Result<GuestConfig> = <TestEngine as QemuBasedEngine>::stage_image_files(
            instance_name,
            &emu_config,
            &None,
            true,
        )
        .await;

        assert!(updated.is_ok(), "expected OK got {:?}", updated.unwrap_err());

        let actual = updated.context("cannot get updated guest config")?;
        let expected = GuestConfig {
            kernel_image: root.join(instance_name).join("kernel"),
            zbi_image: root.join(instance_name).join("zbi"),
            fvm_image: Some(root.join(instance_name).join("fvm")),
        };
        assert_eq!(actual, expected);

        // Test reuse. Note that the ZBI file isn't actually copied in the test, since we replace
        // the ZBI tool with an "echo" command.
        write_to(&emu_config.guest.kernel_image, UPDATED)
            .context("cannot write updated value to kernel file")?;
        write_to(emu_config.guest.fvm_image.as_ref().unwrap(), UPDATED)
            .context("cannot write updated value to fvm file")?;

        let updated = <TestEngine as QemuBasedEngine>::stage_image_files(
            instance_name,
            &emu_config,
            &None,
            true,
        )
        .await;

        assert!(updated.is_ok(), "expected OK got {:?}", updated.unwrap_err());

        let actual = updated.context("cannot get updated guest config, reuse")?;
        let expected = GuestConfig {
            kernel_image: root.join(instance_name).join("kernel"),
            zbi_image: root.join(instance_name).join("zbi"),
            fvm_image: Some(root.join(instance_name).join("fvm")),
        };
        assert_eq!(actual, expected);

        println!("Reading contents from {}", actual.kernel_image.display());
        let mut kernel =
            File::open(&actual.kernel_image).context("cannot open reused kernel file for read")?;
        let mut fvm = File::open(&actual.fvm_image.unwrap())
            .context("cannot open reused fvm file for read")?;

        let mut kernel_contents = String::new();
        let mut fvm_contents = String::new();

        kernel
            .read_to_string(&mut kernel_contents)
            .context("cannot read contents of reused kernel file")?;
        fvm.read_to_string(&mut fvm_contents).context("cannot read contents of reused fvm file")?;

        assert_eq!(kernel_contents, ORIGINAL);
        assert_eq!(fvm_contents, ORIGINAL);

        Ok(())
    }

    #[fuchsia_async::run_singlethreaded(test)]
    #[serial_test::serial]
    async fn test_embed_boot_data() -> Result<()> {
        let _env = ffx_config::test_init().await?;
        let temp = tempdir().context("cannot get tempdir")?;
        let mut emu_config = EmulatorConfiguration::default();

        let root = setup(&mut emu_config.guest, &temp).await?;

        let ctx = mock_modules::get_host_tool_context();
        ctx.expect().returning(|_| Ok(PathBuf::from("echo")));

        let src = emu_config.guest.zbi_image;
        let dest = root.join("dest.zbi");
        let mdns_info = MDNSInfo {
            publications: vec![MDNSServiceInfo {
                media: "wired".to_string(),
                perform_probe: false,
                port: 12345,
                service: "_test._udp.".to_string(),
                text: vec!["host:123.11.22.333".to_string(), "ssh:12345".to_string()],
            }],
        };

        <TestEngine as QemuBasedEngine>::embed_boot_data(&src, &dest, &Some(mdns_info)).await?;

        Ok(())
    }

    #[test]
    fn test_validate_net() -> Result<()> {
        // User mode doesn't have specific requirements, so it should return OK.
        let engine = TestEngine::default();
        let mut emu_config = EmulatorConfiguration::default();
        emu_config.host.networking = NetworkingMode::User;
        let result = engine.validate_network_flags(&emu_config);
        assert!(result.is_ok(), "{:?}", result.unwrap_err());

        // No networking returns an error if no console is selected.
        emu_config.host.networking = NetworkingMode::None;
        emu_config.runtime.console = ConsoleType::None;
        let result = engine.validate_network_flags(&emu_config);
        assert!(result.is_err());

        emu_config.runtime.console = ConsoleType::Console;
        let result = engine.validate_network_flags(&emu_config);
        assert!(result.is_ok(), "{:?}", result.unwrap_err());

        emu_config.runtime.console = ConsoleType::Monitor;
        let result = engine.validate_network_flags(&emu_config);
        assert!(result.is_ok(), "{:?}", result.unwrap_err());

        // Tap mode errors if host is Linux and there's no interface, but we can't mock the
        // interface, so we can't test this case yet.
        emu_config.host.networking = NetworkingMode::Tap;

        // Validation runs after configuration is merged with values from PBMs and runtime, so Auto
        // values should already be resolved. If not, that's a failure.
        emu_config.host.networking = NetworkingMode::Auto;
        let result = engine.validate_network_flags(&emu_config);
        assert!(result.is_err());

        Ok(())
    }
}
