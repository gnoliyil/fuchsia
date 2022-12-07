// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::collections::BTreeSet;

use anyhow::ensure;
use assembly_config_schema::{
    AssemblyConfig, BoardInformation, BoardInformationExt, BuildType, FeatureControl,
};
use fidl_fuchsia_logger::MAX_TAGS;

use crate::common::{CompletedConfiguration, ConfigurationBuilder, PackageConfig};

/// ffx config flag for enabling configuring the assembly+structured config example.
const EXAMPLE_ENABLED_FLAG: &str = "assembly_example_enabled";

const BASE_CONSOLE_ALLOWED_TAGS: &[&str] = &[
    "blobfs",
    "console-launcher",
    "device",
    "driver",
    "driver_host2.cm",
    "driver_manager.cm",
    "fshost",
    "fxfs",
    "mdns",
    "minfs",
    "netcfg",
    "netstack",
    "sshd-host",
    "wlan",
];
static_assertions::const_assert!(BASE_CONSOLE_ALLOWED_TAGS.len() <= MAX_TAGS as usize);

const BASE_CONSOLE_DENIED_TAGS: &[&str] = &["NUD", "klog"];

/// Convert the high-level description of product configuration into a series of configuration
/// value files with concrete component tuples.
///
/// Returns a map from components manifest paths to configuration fields.
pub fn define_bootfs_config(
    config: &AssemblyConfig,
    _board_info: Option<&BoardInformation>,
) -> anyhow::Result<PackageConfig> {
    let mut bootfs_patches = PackageConfig::default();

    // Configure the serial console.
    let allowed_log_tags = {
        let mut allowed_log_tags: BTreeSet<_> =
            BASE_CONSOLE_ALLOWED_TAGS.iter().map(|s| s.to_string()).collect();

        let num_product_tags = config.platform.additional_serial_log_tags.len();
        let max_product_tags = MAX_TAGS as usize - BASE_CONSOLE_ALLOWED_TAGS.len();
        ensure!(
            num_product_tags <= max_product_tags,
            "Max {max_product_tags} tags can be forwarded to serial, got {num_product_tags}."
        );
        allowed_log_tags.extend(config.platform.additional_serial_log_tags.iter().cloned());
        allowed_log_tags.into_iter().collect::<Vec<_>>()
    };
    let denied_log_tags: Vec<_> = BASE_CONSOLE_DENIED_TAGS.iter().map(|s| s.to_string()).collect();
    bootfs_patches
        .component("meta/console.cm")
        .field("allowed_log_tags", allowed_log_tags)
        .field("denied_log_tags", denied_log_tags);

    Ok(bootfs_patches)
}

/// Convert the high-level description of product configuration into a series of configuration
/// value files with concrete package/component tuples.
///
/// Returns a map from package names to configuration updates.
pub fn define_configuration(
    config: &AssemblyConfig,
    board_info: Option<&BoardInformation>,
) -> anyhow::Result<CompletedConfiguration> {
    // Generate the bootfs configuration.
    let bootfs_config = define_bootfs_config(config, board_info)?;

    // Create the packages configuration.
    let builder = ConfigurationBuilder::default();
    let builder = define_configuration_inner(config, board_info, builder)?;
    let completed = builder.completed(bootfs_config.components);
    Ok(completed)
}

// Inner implementation, which will be moved to a trait in the next CL.
fn define_configuration_inner(
    config: &AssemblyConfig,
    board_info: Option<&BoardInformation>,
    mut builder: ConfigurationBuilder,
) -> anyhow::Result<ConfigurationBuilder> {
    // Configure the Product Assembly + Structured Config example, if enabled.
    if should_configure_example() {
        // [START example_patches]
        builder
            .package("configured_by_assembly")
            .component("meta/to_configure.cm")
            .field("enable_foo", matches!(config.platform.build_type, BuildType::Eng));
        // [END example_patches]
    }

    // Configure the session URL.
    ensure!(
        config.product.session_url.is_empty()
            || config.product.session_url.starts_with("fuchsia-pkg://"),
        "valid session URLs must start with `fuchsia-pkg://`, got `{}`",
        config.product.session_url
    );
    builder
        .package("session_manager")
        .component("meta/session_manager.cm")
        .field("session_url", config.product.session_url.to_owned());

    // Configure enabling pinweaver.
    {
        let (allow_scrypt, allow_pinweaver) = match (
            board_info.provides_feature("fuchsia::cr50"),
            &config.platform.identity.password_pinweaver,
        ) {
            // if the bpard doesn't support pinweaver:
            // scrypt is always allowed, and pinweaver is not.
            (false, _) => (true, false),
            // if the board supports pinweaver, and pinweaver is required:
            // scrypt is not allowed, and pinweaver is
            (true, FeatureControl::Required) => (false, true),
            // if the board supports pinweaver, and use of pinweaver is allowed:
            // both are allowed
            (true, FeatureControl::Allowed) => (true, true),
            // if the board supports pinweaver, and use of pinweaver is disabled
            // scrypt is allowed, pinweaver is not
            (true, FeatureControl::Disabled) => (true, false),
        };
        builder
            .package("password_authenticator")
            .component("meta/password-authenticator.cm")
            .field("allow_scrypt", allow_scrypt)
            .field("allow_pinweaver", allow_pinweaver);
    }

    // Configure the input interaction activity service.
    const DEFAULT_IDLE_THRESHOLD_MINUTES: u64 = 15;
    builder.package("input-pipeline").component("meta/input-pipeline.cm").field(
        "idle_threshold_minutes",
        config.platform.input.idle_threshold_minutes.unwrap_or(DEFAULT_IDLE_THRESHOLD_MINUTES),
    );
    let scene_manager_config = builder.package("scene_manager").component("meta/scene_manager.cm");
    scene_manager_config.field(
        "idle_threshold_minutes",
        config.platform.input.idle_threshold_minutes.unwrap_or(DEFAULT_IDLE_THRESHOLD_MINUTES),
    );

    // Configure the supported input devices. Default to an empty list.
    scene_manager_config.field(
        "supported_input_devices",
        config
            .platform
            .input
            .supported_input_devices
            .iter()
            .filter_map(|d| serde_json::to_value(d).ok())
            .collect::<serde_json::Value>(),
    );

    Ok(builder)
}

/// Check ffx config for whether we should execute example code.
fn should_configure_example() -> bool {
    futures::executor::block_on(ffx_config::get::<bool, _>(EXAMPLE_ENABLED_FLAG))
        .unwrap_or_default()
}
