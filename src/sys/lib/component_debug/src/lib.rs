// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! This library provides methods for getting information about components in Fuchsia.

pub mod capability;
pub mod cli;
pub mod copy;
pub mod doctor;
pub mod explore;
pub mod io;
pub mod lifecycle;
pub mod list;
pub mod path;
pub mod query;
pub mod realm;
pub mod show;
pub mod storage;

#[cfg(test)]
pub mod test_utils;
