// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![warn(clippy::all)]

pub(crate) mod common;
pub(crate) mod subsystems;

pub use common::CompletedConfiguration;
pub use common::ComponentConfigs;
pub use common::PackageConfigs;
pub use common::PackageConfiguration;
pub use subsystems::define_configuration;
