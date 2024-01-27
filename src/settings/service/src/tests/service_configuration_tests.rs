// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::collections::HashSet;
use std::sync::Arc;

use fidl_fuchsia_settings::{AccessibilityMarker, DisplayMarker, PrivacyMarker};

use fidl_fuchsia_settings_policy::VolumePolicyControllerMarker;

use crate::config::default_settings::DefaultSetting;
use crate::ingress::fidl::InterfaceSpec;
use crate::storage::testing::InMemoryStorageFactory;
use crate::AgentConfiguration;
use crate::EnabledInterfacesConfiguration;
use crate::EnvironmentBuilder;
use crate::ServiceConfiguration;
use crate::ServiceFlags;

const ENV_NAME: &str = "settings_service_configuration_test_environment";

fn get_test_interface_specs() -> HashSet<InterfaceSpec> {
    [InterfaceSpec::Accessibility, InterfaceSpec::Privacy].into()
}

#[fuchsia::test(allow_stalls = false)]
async fn test_no_configuration_provided() {
    let factory = InMemoryStorageFactory::new();

    let default_configuration =
        EnabledInterfacesConfiguration::with_interfaces(get_test_interface_specs());

    let flags = ServiceFlags::default();
    let configuration =
        ServiceConfiguration::from(AgentConfiguration::default(), default_configuration, flags);

    let env = EnvironmentBuilder::new(Arc::new(factory))
        .configuration(configuration)
        .spawn_and_get_protocol_connector(ENV_NAME)
        .await
        .unwrap();

    // No ServiceConfiguration provided, we should be able to connect to the service and make a watch call without issue.
    let service = env.connect_to_protocol::<AccessibilityMarker>().expect("Connected to service");
    let _ = service.watch().await.expect("watch completed");

    // No ServiceConfiguration provided, audio policy should not be able to connect.
    let policy = env
        .connect_to_protocol::<VolumePolicyControllerMarker>()
        .expect("Connected to policy service");
    let _ = policy.get_properties().await.expect_err("Policy get should fail");
}

#[fuchsia::test(allow_stalls = false)]
async fn test_default_interfaces_configuration_provided() {
    let factory = InMemoryStorageFactory::new();

    // Load test configuration, which only has Display, default will not be used.
    let configuration = DefaultSetting::new(None, "/config/data/interface_configuration.json")
        .load_default_value()
        .expect("invalid interface configuration")
        .expect("no enabled interface configuration provided");

    let flags = ServiceFlags::default();
    let configuration =
        ServiceConfiguration::from(AgentConfiguration::default(), configuration, flags);

    let env = EnvironmentBuilder::new(Arc::new(factory))
        .configuration(configuration)
        .spawn_and_get_protocol_connector(ENV_NAME)
        .await
        .unwrap();

    let _ = env.connect_to_protocol::<DisplayMarker>().expect("Connected to service");

    // Any calls to the privacy service should fail since the service isn't included in the configuration.
    let privacy_service = env.connect_to_protocol::<PrivacyMarker>().unwrap();
    let _ = privacy_service.watch().await.expect_err("watch completed");
}
