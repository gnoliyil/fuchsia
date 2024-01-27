// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use serde::{Deserialize, Serialize};

/// Platform configuration options for the graphics are.
#[derive(Debug, Default, Deserialize, Serialize, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct GraphicsConfig {
    /// Whether the virtual console should be included.  This has a different
    /// default value depending on the BuildType.  It's 'true' for Eng and
    /// UserDebug, false for User.
    #[serde(default)]
    pub enable_virtual_console: Option<bool>,
}
