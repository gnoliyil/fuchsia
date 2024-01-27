// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use serde::{Deserialize, Serialize};

/// Configuration options for how to act when a driver host crashes.
#[derive(Debug, Deserialize, Serialize, PartialEq)]
#[serde(deny_unknown_fields, rename_all = "kebab-case")]
pub enum DriverHostCrashPolicy {
    RestartDriverHost,
    RebootSystem,
    DoNothing,
}

impl std::fmt::Display for DriverHostCrashPolicy {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            DriverHostCrashPolicy::RestartDriverHost => write!(f, "restart-driver-host"),
            DriverHostCrashPolicy::RebootSystem => write!(f, "reboot-system"),
            DriverHostCrashPolicy::DoNothing => write!(f, "do-nothing"),
        }
    }
}

/// Platform configuration options for driver framework support.
#[derive(Debug, Default, Deserialize, Serialize, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct DriverFrameworkConfig {
    #[serde(default)]
    pub eager_drivers: Vec<String>,

    #[serde(default)]
    pub disabled_drivers: Vec<String>,

    #[serde(default)]
    pub driver_host_crash_policy: Option<DriverHostCrashPolicy>,
}
