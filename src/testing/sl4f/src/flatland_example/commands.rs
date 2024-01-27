// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::server::Facade;
use anyhow::Error;
use async_trait::async_trait;
use serde_json::Value;

use super::facade::FlatlandExampleFacade;
use super::types::FlatlandExampleMethod;

#[async_trait(?Send)]
impl Facade for FlatlandExampleFacade {
    async fn handle_request(&self, method: String, _args: Value) -> Result<Value, Error> {
        match method.parse()? {
            FlatlandExampleMethod::Start => self.start().await,
            FlatlandExampleMethod::Stop => self.stop().await,
        }
    }
}
