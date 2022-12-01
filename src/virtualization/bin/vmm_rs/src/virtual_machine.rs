// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use fidl_fuchsia_virtualization::{GuestConfig, GuestError};

pub trait VirtualMachine: Sized {
    fn new(_config: GuestConfig) -> Result<Self, GuestError>;
    fn start_primary_vcpu(&self) -> Result<(), GuestError>;
}

pub struct FuchsiaVirtualMachine;

impl VirtualMachine for FuchsiaVirtualMachine {
    fn new(_config: GuestConfig) -> Result<Self, GuestError> {
        unimplemented!();
    }

    fn start_primary_vcpu(&self) -> Result<(), GuestError> {
        unimplemented!();
    }
}
