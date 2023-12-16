// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
use anyhow::Result;
use fidl_fuchsia_power_broker as fbroker;
use fuchsia_zircon::{HandleBased, Rights};

pub struct PowerElementContext {
    pub element_control: fbroker::ElementControlProxy,
    pub lessor: fbroker::LessorProxy,
    pub level_control: fbroker::LevelControlProxy,
    dependency_token: fbroker::DependencyToken,
}

impl PowerElementContext {
    pub async fn new(
        topology: &fbroker::TopologyProxy,
        element_name: &str,
        initial_current_level: &fbroker::PowerLevel,
        default_level: &fbroker::PowerLevel,
        dependencies: Vec<fbroker::LevelDependency>,
        mut dependency_tokens_to_register: Vec<fbroker::DependencyToken>,
    ) -> Result<Self> {
        let dependency_token = fbroker::DependencyToken::create();
        dependency_tokens_to_register.push(
            dependency_token
                .duplicate_handle(Rights::SAME_RIGHTS)
                .expect("failed to duplicate token"),
        );

        let (element_control_client_end, lessor_client_end, level_control_client_end) = topology
            .add_element(
                element_name,
                initial_current_level,
                default_level,
                dependencies,
                dependency_tokens_to_register,
            )
            .await?
            .map_err(|d| anyhow::anyhow!("{d:?}"))?;

        Ok(Self {
            element_control: element_control_client_end.into_proxy()?,
            lessor: lessor_client_end.into_proxy()?,
            level_control: level_control_client_end.into_proxy()?,
            dependency_token,
        })
    }

    pub fn dependency_token(&self) -> fbroker::DependencyToken {
        self.dependency_token
            .duplicate_handle(Rights::SAME_RIGHTS)
            .expect("failed to duplicate token")
    }
}
