// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Component bedrock capabilities.

mod cap;
pub mod dict;
pub mod handle;
pub mod multishot;

pub use self::cap::{AnyCapability, AnyCloneCapability, Capability, CloneCapability, Remote};
pub use self::handle::{CloneHandle, Handle};
pub use self::multishot::multishot;

pub type CloneDict = dict::Dict<AnyCloneCapability>;
pub type Dict = dict::Dict<AnyCapability>;
