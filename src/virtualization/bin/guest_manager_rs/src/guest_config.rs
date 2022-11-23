// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TODO(fxbug.dev/115695): Remove.
#![allow(unused_variables, unused_imports, dead_code)]

use {
    anyhow::{anyhow, Error},
    fidl_fuchsia_virtualization::GuestConfig,
};

pub fn parse_config(data: &str) -> Result<GuestConfig, Error> {
    // Parse a guest config file into a GuestConfig struct. See the C++ ParseConfig for an example,
    // but don't try and replicate the C++ logic. Rust provides JSON parsing via the serde crate.
    // TODO(fxbug.dev/115695): Implement this function and remove this comment.
    unimplemented!();
}

pub fn merge_configs(base: GuestConfig, overrides: GuestConfig) -> GuestConfig {
    // Merge two configs, with the overrides being applied on top of the base config. Non-repeated
    // fields should be overwritten, and repeated fields should be appended. See the C++
    // MergeConfigs for an example.
    // TODO(fxbug.dev/115695): Implement this function and remove this comment.
    unimplemented!();
}

#[cfg(test)]
mod tests {
    use super::*;

    #[fuchsia::test]
    async fn parse_empty_config() {
        // Test parsing an empty JSON string.
        // TODO(fxbug.dev/115695): Write this test and remove this comment.
    }

    #[fuchsia::test]
    async fn parse_config() {
        // Parse a config without embedded JSON.
        // TODO(fxbug.dev/115695): Write this test and remove this comment.
    }

    #[fuchsia::test]
    async fn merge_simple_configs() {
        // Merge two configs without repeated fields.
        // TODO(fxbug.dev/115695): Write this test and remove this comment.
    }

    #[fuchsia::test]
    async fn merge_configs_with_arrays() {
        // Merge two configs with repeated fields appended.
        // TODO(fxbug.dev/115695): Write this test and remove this comment.
    }
}
