// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::convert::From;
use tpm2_tss_sys as tss_sys;

/// `TPM_PT` defining the properties associated with a TPM_CAP_TPM_PROPERTIES
/// capability type.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Tpm2Property {
    /// TPM_PT_MANUFACTURER, a string describing the manufacturer.
    Manufacturer,
}

/// Converts a `Tpm2Property` into its underlying `tss_sys::TPM2_PT` type.
impl From<&Tpm2Property> for tss_sys::TPM2_PT {
    fn from(item: &Tpm2Property) -> tss_sys::TPM2_PT {
        match item {
            Tpm2Property::Manufacturer => tss_sys::TPM2_PT_MANUFACTURER,
        }
    }
}

/// `TPM2_CAP` enum of the available TPM2_CAP variants, codified as a real type
/// instead of just an arbitrary integer.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Tpm2Capability {
    /// TPM_CAP_TPM_PROPERTIES returns a list of tagged properties.
    Tpm2Properties(Tpm2Property),
}

/// Converts a `Tpm2Capability` into its underlying `tss_sys::TPM2_CAP` type.
impl From<&Tpm2Capability> for tss_sys::TPM2_CAP {
    fn from(item: &Tpm2Capability) -> tss_sys::TPM2_CAP {
        match item {
            Tpm2Capability::Tpm2Properties(_) => tss_sys::TPM2_CAP_TPM_PROPERTIES,
        }
    }
}

/// Returns the property associtated with the capability as an underlying u32
/// so it can interop with the tss_sys API.
pub(crate) fn capability_property(capability: &Tpm2Capability) -> u32 {
    match capability {
        Tpm2Capability::Tpm2Properties(property) => property.into(),
    }
}

#[cfg(test)]
mod tests {}
