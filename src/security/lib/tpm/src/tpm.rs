// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::capability::{Tpm2Capability, Tpm2Property};
use crate::error::TpmError;
use crate::tss::{EsysContext, TctiContext};

/// Represents the ownership status of the TPM. This simply indicates whether
/// the owner authorization password has been set.
pub enum TpmOwnership {
    Owned,
    Unowned,
}

/// The `Tpm` is the interface to the `Trusted Platform Module 2.0`. The
/// intention of this interface is to provide a small surface area of
/// methods limited to just the Fuchsia projects requirements that importantly
/// are not 1:1 mappings to TPM commands but are instead mapped around
/// features such as provisioning, deprovisioning etc. This reduces the chance
/// of misuse and abstracts the underlying C implementation and the memory
/// allocation complications of that from the consumer.
#[allow(dead_code)]
pub struct Tpm {
    /// The Enhanced System API (esys) context contains all the bookkeeping
    /// about the current TPM device or agent we are connected to.
    esys_context: EsysContext,
}

impl Tpm {
    /// Attempts to connect to the TPM by establishing a connection with the
    /// TPM Command Transmission Interface (TCTI). It then attempts to use
    /// that TCTI connection to construct an Enhanced System API interface
    /// (ESYS) which is used to service commands.
    pub fn try_new() -> Result<Self, TpmError> {
        let tcti_context = TctiContext::try_new()?;
        let esys_context = EsysContext::try_new(tcti_context)?;
        Ok(Self { esys_context })
    }

    /// Calls TPM2_GetRandom Returns `bytes_requested` random bytes from the
    /// TPM.
    pub fn get_random(&self, bytes_requested: usize) -> Result<Vec<u8>, TpmError> {
        self.esys_context.get_random(bytes_requested)
    }

    /// Calls TPM2_Startup passing in SU_CLEAR.
    pub fn startup(&self) -> Result<(), TpmError> {
        self.esys_context.startup()
    }

    /// Calls TPM2_Clear. This assumes the command has not been disabled by
    /// TPM2_ClearControl and that the platform auth is default.
    pub fn clear(&self) -> Result<(), TpmError> {
        self.esys_context.clear()
    }

    /// Returns the manufacturer string defined by `TPM2_PT_MANUFACTURER` in
    /// the specification.
    pub fn manufacturer(&self) -> Result<u32, TpmError> {
        let capability = self
            .esys_context
            .get_capability(&Tpm2Capability::Tpm2Properties(Tpm2Property::Manufacturer), 1)?;
        // Accessing a union is unsafe in Rust. This should always be populated
        // by the underlying C code assuming the command above has succeeded.
        unsafe {
            if capability.data.tpmProperties.count != 1 {
                return Err(TpmError::UnexpectedPropertyCount {
                    expected: 1,
                    found: capability.data.tpmProperties.count,
                });
            }
            Ok(capability.data.tpmProperties.tpmProperty[0].value)
        }
    }
}
