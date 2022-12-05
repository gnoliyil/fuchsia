// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use tpm2_tss_sys as tss_sys;

/// Typed representation of the `TPM2_RH` type.
pub enum Tpm2Hierarchy {
    Endorsement,
    Owner,
    Platform,
    Null,
}

/// Converts the `TPM2_RH` type into the underlying TSS type.
impl From<&Tpm2Hierarchy> for tss_sys::TPM2_RH {
    fn from(item: &Tpm2Hierarchy) -> tss_sys::TPM2_RH {
        match item {
            Tpm2Hierarchy::Endorsement => tss_sys::TPM2_RH_ENDORSEMENT,
            Tpm2Hierarchy::Owner => tss_sys::TPM2_RH_OWNER,
            Tpm2Hierarchy::Platform => tss_sys::TPM2_RH_PLATFORM,
            Tpm2Hierarchy::Null => tss_sys::TPM2_RH_NULL,
        }
    }
}
