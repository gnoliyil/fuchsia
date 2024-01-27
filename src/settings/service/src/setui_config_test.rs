// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::{Context, Error};
use argh::FromArgs;
use serde::de::DeserializeOwned;
use settings::{
    AgentConfiguration, DisplayConfiguration, EnabledInterfacesConfiguration, InputConfiguration,
    LightHardwareConfiguration, LightSensorConfig, ServiceFlags,
};
use std::collections::hash_map::DefaultHasher;
use std::collections::HashMap;
use std::ffi::{OsStr, OsString};
use std::fs::File;
use std::hash::{Hash, Hasher};
use std::io::Read;

/// setui_config_tests validates configuration files passed to the settings service.
#[derive(FromArgs)]
struct TestConfig {
    /// these configurations are the one that will determine the initial
    /// display settings.
    #[argh(option, short = 'd')]
    display_config: Vec<OsString>,

    /// these configurations are the one that will determine the behavior of individual controllers.
    #[argh(option, short = 'f')]
    controller_flags: Vec<OsString>,

    /// these configurations control the default input devices for a product.
    #[argh(option, short = 'i')]
    input_device_config: Vec<OsString>,

    /// these configurations control which interfaces are enabled.
    #[argh(option, short = 'x')]
    interface_config: Vec<OsString>,

    /// these configurations control specific settings within the light sensor controller.
    #[argh(option, short = 'l')]
    light_sensor_config: Vec<OsString>,

    /// these configurations control specific settings for light hardware.
    #[argh(option, short = 'h')]
    light_hardware_config: Vec<OsString>,

    /// these configurations control which agents are enabled.
    #[argh(option, short = 'a')]
    agent_config: Vec<OsString>,

    /// these configurations add audio policy transforms at build-time.
    #[argh(option)]
    audio_policy_config: Vec<OsString>,
}

fn read_config<C: DeserializeOwned>(path: &OsStr) -> Result<C, Error> {
    tracing::info!("Validating {:?}", path);
    let mut file = File::open(path)
        .with_context(|| format!("Couldn't open path `{}`", path.to_string_lossy()))?;
    let mut contents = String::new();
    let _: usize = file
        .read_to_string(&mut contents)
        .with_context(|| format!("Couldn't read file at path `{}`", path.to_string_lossy()))?;
    Ok(serde_json::from_str::<C>(&contents).context("Failed to deserialize flag configuration")?)
}

fn main() -> Result<(), Error> {
    let test_config: TestConfig = argh::from_env();

    for config in test_config.display_config.into_iter() {
        let _ = read_config::<DisplayConfiguration>(&config)?;
    }

    for config in test_config.controller_flags.into_iter() {
        let _ = read_config::<ServiceFlags>(&config)?;
    }

    for config in test_config.input_device_config.into_iter() {
        let _ = read_config::<InputConfiguration>(&config)?;
    }

    for config in test_config.interface_config.into_iter() {
        let _ = read_config::<EnabledInterfacesConfiguration>(&config)?;
    }

    for config in test_config.light_sensor_config.into_iter() {
        let _ = read_config::<LightSensorConfig>(&config)?;
    }

    for config in test_config.light_hardware_config.into_iter() {
        let light_config: LightHardwareConfiguration =
            read_config::<LightHardwareConfiguration>(&config)?;

        // Verify that no two light group names hash to the same value. This guarantee is important
        // for the light service to function properly. This also implicitly prevents two light
        // groups from having the same name.

        let mut hash_to_name: HashMap<u64, String> = HashMap::new();
        for light_group in light_config.light_groups {
            // Hash the name.
            let mut hasher = DefaultHasher::new();
            light_group.name.hash(&mut hasher);
            let hash = hasher.finish();

            // Insert returns the previous value at the given key, if any. If there was a value with
            // the same hash, panic.
            if let Some(conflicting_name) = hash_to_name.insert(hash, light_group.name.clone()) {
                panic!("{:?} has the same hash as {:?}", light_group.name, conflicting_name);
            }
        }
    }

    for config in test_config.agent_config.into_iter() {
        let _ = read_config::<AgentConfiguration>(&config)?;
    }

    for _config in test_config.audio_policy_config.into_iter() {
        // Audio policy support has been deprecated.
    }

    Ok(())
}
