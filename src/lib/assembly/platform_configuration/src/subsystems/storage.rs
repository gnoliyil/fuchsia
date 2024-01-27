// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::subsystems::prelude::*;
use assembly_config_schema::platform_config::storage_config::StorageConfig;
use assembly_config_schema::FeatureSupportLevel;

pub(crate) struct StorageSubsystemConfig;
impl DefineSubsystemConfiguration<StorageConfig> for StorageSubsystemConfig {
    fn define_configuration(
        context: &ConfigurationContext<'_>,
        storage_config: &StorageConfig,
        builder: &mut dyn ConfigurationBuilder,
    ) -> anyhow::Result<()> {
        if matches!(context.feature_set_level, FeatureSupportLevel::Empty) {
            return Ok(());
        }

        if storage_config.live_usb_enabled {
            builder.platform_bundle("live_usb");
        } else {
            builder.platform_bundle("empty_live_usb");
        }

        Ok(())
    }
}
