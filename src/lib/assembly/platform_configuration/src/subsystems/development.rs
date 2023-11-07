// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::subsystems::prelude::*;
use anyhow::Context;
use assembly_config_schema::platform_config::development_support_config::DevelopmentSupportConfig;

pub(crate) struct DevelopmentConfig;
impl DefineSubsystemConfiguration<DevelopmentSupportConfig> for DevelopmentConfig {
    fn define_configuration(
        context: &ConfigurationContext<'_>,
        config: &DevelopmentSupportConfig,
        builder: &mut dyn ConfigurationBuilder,
    ) -> anyhow::Result<()> {
        // Select the correct AIB based on the user-provided setting if present
        // and fall-back to the default by build-type.
        builder.platform_bundle(match (context.build_type, config.enabled) {
            (BuildType::User, Some(_)) => {
                anyhow::bail!("Development support cannot be enabled on user builds");
            }

            // User-provided development setting for non-user builds.
            (_, Some(true)) => "kernel_args_eng",
            (_, Some(false)) => "kernel_args_user",

            // Default development setting by build-type.
            (BuildType::Eng, None) => "kernel_args_eng",
            (BuildType::UserDebug, None) => "kernel_args_userdebug",
            (BuildType::User, None) => "kernel_args_user",
        });

        match (context.build_type, &config.authorized_ssh_keys_path) {
            (BuildType::User, Some(_)) => {
                anyhow::bail!("authorized_ssh_keys cannot be provided on user builds")
            }
            (_, Some(authorized_ssh_keys_path)) => {
                builder
                    .package("sshd-host")
                    .config_data(assembly_config_schema::FileEntry {
                        source: authorized_ssh_keys_path.clone(),
                        destination: "authorized_keys".into(),
                    })
                    .context("Setting authorized_keys")?;
            }
            _ => {}
        }

        match (context.build_type, &config.authorized_ssh_ca_certs_path) {
            (BuildType::User, Some(_)) => {
                anyhow::bail!("authorized_ssh_ca_certs_path cannot be provided on user builds")
            }
            (_, Some(authorized_ssh_ca_certs_path)) => {
                builder
                    .package("sshd-host")
                    .config_data(assembly_config_schema::FileEntry {
                        source: authorized_ssh_ca_certs_path.clone(),
                        destination: "ssh_ca_pub_keys".into(),
                    })
                    .context("Setting authorized ssh ca certs")?;
            }
            _ => {}
        }

        if config.include_sl4f {
            builder.platform_bundle("sl4f");
        }

        match (context.build_type, &config.include_bin_clock) {
            (BuildType::User, true) => {
                anyhow::bail!("bin/clock cannot be provided on user builds")
            }
            (BuildType::Eng, _) | (BuildType::UserDebug, true) => {
                builder.platform_bundle("clock_development_tools")
            }
            (BuildType::User, false) | (BuildType::UserDebug, false) => {}
        }

        Ok(())
    }
}
