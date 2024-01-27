// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! The show_output module contains print routines for the Show subcommand.

use camino::Utf8PathBuf;
use emulator_instance::{EmulatorConfiguration, NetworkingMode};
use ffx_emulator_config::ShowDetail;
use sdk_metadata::{
    virtual_device::{Cpu, Hardware},
    ElementType, InputDevice, VirtualDeviceV1,
};
use std::{collections::HashMap, process::Command};

pub(crate) fn command(cmd: &Command) -> ShowDetail {
    let mut env: HashMap<String, String> = HashMap::new();
    for (k, v) in cmd.get_envs() {
        if let Some(value) = v {
            env.insert(k.to_string_lossy().to_string(), value.to_string_lossy().to_string());
        }
    }
    ShowDetail::Cmd {
        program: Some(String::from(cmd.get_program().to_string_lossy())),
        args: Some(cmd.get_args().into_iter().map(|a| String::from(a.to_string_lossy())).collect()),
        env: Some(env),
    }
}
pub(crate) fn net(emu_config: &EmulatorConfiguration) -> ShowDetail {
    match emu_config.host.networking {
        NetworkingMode::Tap =>
            ShowDetail::Net {
                mode: Some(emu_config.host.networking.clone()),
                mac_address: Some(emu_config.runtime.mac_address.clone()),
                upscript: emu_config.runtime.upscript.clone(),
                ports: None
            },
        NetworkingMode::User =>
            ShowDetail::Net {
                mode: Some(emu_config.host.networking.clone()),
                mac_address: Some(emu_config.runtime.mac_address.clone()),
                upscript: None,
                ports: Some(emu_config.host.port_map.clone())
            },
        NetworkingMode::Auto |  /* Auto will already be resolved, so skip */
        NetworkingMode::None =>
           ShowDetail::Net {
            mode: Some(emu_config.host.networking.clone()),
            mac_address: None,
            upscript: None,
            ports: None
        }
    }
}

pub(crate) fn config(emu_config: &EmulatorConfiguration) -> ShowDetail {
    ShowDetail::Config { flags: Some(emu_config.flags.clone()) }
}

pub(crate) fn device(emu_config: &EmulatorConfiguration) -> ShowDetail {
    let mut device = VirtualDeviceV1 {
        name: format!("{}_device", emu_config.runtime.name),
        description: Some(format!(
            "The virtual device used to launch the {} emulator.",
            emu_config.runtime.name
        )),
        kind: ElementType::VirtualDevice,
        hardware: Hardware {
            cpu: Cpu { arch: emu_config.device.cpu.architecture.clone() },
            audio: emu_config.device.audio.clone(),
            storage: emu_config.device.storage.clone(),
            inputs: InputDevice { pointing_device: emu_config.device.pointing_device.clone() },
            memory: emu_config.device.memory.clone(),
            window_size: emu_config.device.screen.clone(),
        },
        start_up_args_template: Utf8PathBuf::from_path_buf(emu_config.runtime.template.clone())
            .expect("Converting to utf8"),
        ports: None,
    };

    let mut ports = HashMap::new();
    for (name, mapping) in &emu_config.host.port_map {
        ports.insert(name.clone(), mapping.guest);
    }
    if !ports.is_empty() {
        device.ports = Some(ports.clone());
    }

    ShowDetail::Device { device: Some(device) }
}
