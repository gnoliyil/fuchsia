// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::subsystems::prelude::*;
use anyhow::bail;
use assembly_config_schema::platform_config::connectivity_config::{
    NetworkingConfig, PlatformConnectivityConfig,
};

pub(crate) struct ConnectivitySubsystemConfig;
impl DefineSubsystemConfiguration<PlatformConnectivityConfig> for ConnectivitySubsystemConfig {
    fn define_configuration(
        context: &ConfigurationContext<'_>,
        connectivity_config: &PlatformConnectivityConfig,
        builder: &mut dyn ConfigurationBuilder,
    ) -> anyhow::Result<()> {
        // The configuration of networking is dependent on all three of:
        // - the feature_set_level
        // - the build_type
        // - the requested configuration type
        let networking = match (
            context.feature_set_level,
            context.build_type,
            &connectivity_config.network.networking,
        ) {
            // bootstrap must not attempt to configure it, it's always None
            (FeatureSupportLevel::Bootstrap, _, Some(_)) => {
                bail!("The configuration of networking is not an option for `bootstrap`")
            }
            (FeatureSupportLevel::Bootstrap, _, None) => None,

            // utility, in user mode, only gets networking if requested.
            (FeatureSupportLevel::Utility, BuildType::User, networking) => networking.as_ref(),

            // all other combinations get the network package that they request
            (_, _, Some(networking)) => Some(networking),

            // otherwise, the 'standard' networking package is used
            (_, _, None) => Some(&NetworkingConfig::Standard),
        };
        if let Some(_networking) = networking {
            // The 'core_realm_networking' bundle is required if networking is
            // enabled.
            builder.platform_bundle("core_realm_networking");

            //TODO(fxbug.dev/122862) - Include the networking bundles when they
            //are ready to be included.
            //
            // // Which specific network package is selectable by the product.
            // match networking {
            //     NetworkingConfig::Standard => {
            //         builder.platform_bundle("networking");
            //     }
            //     NetworkingConfig::Basic => {
            //         builder.platform_bundle("networking-basic");
            //     }
            // }

            // The use of netstack3 can be forcibly required by the board,
            // otherwise it's selectable by the product.
            if context.board_info.provides_feature("fuchsia::network_require_netstack3")
                || connectivity_config.network.force_netstack3
            {
                builder.platform_bundle("netstack3");
            } else {
                builder.platform_bundle("netstack2");
            }

            let has_fullmac = context.board_info.provides_feature("fuchsia::wlan_fullmac");
            let has_softmac = context.board_info.provides_feature("fuchsia::wlan_softmac");
            if context.feature_set_level == &FeatureSupportLevel::Minimal
                && (has_fullmac || has_softmac)
            {
                builder.platform_bundle("wlan_base");
                // Some products require legacy security types to be supported.
                // Otherwise, they are disabled by default.
                if connectivity_config.wlan.legacy_privacy_support {
                    builder.platform_bundle("wlan_legacy_privacy_support");
                } else {
                    builder.platform_bundle("wlan_contemporary_privacy_only_support");
                }

                if has_fullmac {
                    builder.platform_bundle("wlan_fullmac_support");
                }
                if has_softmac {
                    builder.platform_bundle("wlan_softmac_support");
                }
            }
        }

        Ok(())
    }
}
