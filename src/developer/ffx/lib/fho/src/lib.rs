// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

pub mod adapters;
mod from_env;
mod metadata;
mod search;
mod subtool;
pub mod testing;

pub use from_env::*;
pub use metadata::*;
pub use search::*;
pub use subtool::*;

// Used for deriving an FFX tool.
pub use fho_macro::FfxTool;

// Re-expose the Error, Result, and FfxContext types from ffx_command
// so you don't have to pull both in all the time.
pub use ffx_command::{Error, FfxContext, Result};

// Re-expose the ffx_writer::Writer as the 'simple writer'
pub use ffx_writer::Writer as SimpleWriter;

#[doc(hidden)]
pub mod macro_deps {
    pub use anyhow;
    pub use argh;
    pub use async_trait::async_trait;
    pub use ffx_command::{Ffx, FfxCommandLine, ToolRunner};
    pub use ffx_config::{global_env_context, EnvironmentContext};
    pub use ffx_core::Injector;
    pub use futures;
    pub use serde;
}
