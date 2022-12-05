// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::{
    capability::{capability_property, Tpm2Capability},
    error::TpmError,
    tss::{heap::EsysHeapWrapper, tcti::TctiContext},
};
use std::ptr::null_mut;
use tpm2_tss_sys as tss_sys;

/// The number of session handles that the ESYS TPM2.0 can support for most
/// of its functions. This is used to simplify session management.
const NUM_SESSION_HANDLES: usize = 3;

/// Safe wrapper around the `ESYS_CONTEXT`. The Enhanced System API Context
/// hosts all the bookkeeping the TSS2 library. The `EsysContext` must own
/// the lifetime of the `TctiContext` however it is not responsible for
/// freeing the `TctiContext` which is the same behavior as in the C API.
/// TctiContext defines its own free mechanism so when the context goes out
/// of scope it will be freed after we finalize the `EsysContext`.
pub struct EsysContext {
    _tcti_context: TctiContext,
    esys_context: *mut tss_sys::ESYS_CONTEXT,
    session_handles: [u32; NUM_SESSION_HANDLES],
}

impl EsysContext {
    /// Attempts initialize the TSS2 Enhanced System API Context with the
    /// provided `tcti_context`. `EsysContext` takes ownership of the lifetime
    /// of the `TctiContext`.
    pub fn try_new(tcti_context: TctiContext) -> Result<Self, TpmError> {
        let mut esys_context = null_mut();
        let return_code = unsafe {
            tss_sys::Esys_Initialize(&mut esys_context, tcti_context.as_mut_ptr(), null_mut())
        };
        if return_code != 0 {
            return Err(TpmError::TssReturnCode(return_code));
        }
        Ok(Self {
            _tcti_context: tcti_context,
            esys_context,
            session_handles: [tss_sys::ESYS_TR_NONE, tss_sys::ESYS_TR_NONE, tss_sys::ESYS_TR_NONE],
        })
    }

    /// Returns the first session handle intended for slot one.
    fn session_handle_one(&self) -> u32 {
        self.session_handles[0]
    }

    /// Returns the first session handle intended for slot two.
    fn session_handle_two(&self) -> u32 {
        self.session_handles[1]
    }

    /// Returns the first session handle intended for slot three.
    fn session_handle_three(&self) -> u32 {
        self.session_handles[2]
    }

    /// This function invokes TPM2_Startup with the SU_CLEAR param.
    /// See: Trusted Platform Module Library, Part 3: Commands,
    ///      Subsection 9.3.1.
    pub fn startup(&self) -> Result<(), TpmError> {
        // By default you want to call clear and not
        let return_code =
            unsafe { tss_sys::Esys_Startup(self.esys_context, tss_sys::TPM2_SU_CLEAR) };
        if return_code != 0 {
            return Err(TpmError::TssReturnCode(return_code));
        }
        Ok(())
    }

    /// This function invokes TPM2_Clear which attempts to clear the TPM.
    /// See: Trusted Platform Module Library, Part 3: Commands,
    ///      Subsection 24.6.1
    pub fn clear(&self) -> Result<(), TpmError> {
        let return_code = unsafe {
            tss_sys::Esys_Clear(
                self.esys_context,
                tss_sys::ESYS_TR_RH_PLATFORM,
                tss_sys::ESYS_TR_PASSWORD,
                tss_sys::ESYS_TR_NONE,
                tss_sys::ESYS_TR_NONE,
            )
        };
        if return_code != 0 {
            return Err(TpmError::TssReturnCode(return_code));
        }
        Ok(())
    }

    /// Calls `TPM2_GetRandom` returning `bytes_requested` of random bytes from
    /// the TPM. Sizes larger than the max payload of `TPM2_GetRandom` will be
    /// batched and extended onto each other.
    /// See: Trusted Platform Module Library, Part 3: Commands,
    ///      Subsection: 16.1.1
    pub fn get_random(&self, bytes_requested: usize) -> Result<Vec<u8>, TpmError> {
        if bytes_requested == 0 {
            return Ok(vec![]);
        }
        // For requests greater than the max payload batch them.
        let mut random_bytes: Vec<u8> = Vec::new();
        let mut remaining_bytes = bytes_requested;
        let max_payload = std::mem::size_of::<tss_sys::TPMU_HA>();
        while remaining_bytes > max_payload {
            // This frees the buffer on each iteration of the loop. Which is
            // required as each call performs an allocation.
            let mut digest = EsysHeapWrapper::<tss_sys::TPM2B_DIGEST>::null();
            let return_code = unsafe {
                tss_sys::Esys_GetRandom(
                    self.esys_context,
                    self.session_handle_one(),
                    self.session_handle_two(),
                    self.session_handle_three(),
                    bytes_requested.try_into().expect("this should always be bounded by u16"),
                    &mut digest.inner,
                )
            };
            if return_code != 0 {
                return Err(TpmError::TssReturnCode(return_code));
            }
            random_bytes.extend(unsafe { (*digest.inner).buffer });
            remaining_bytes -= max_payload;
        }
        let mut remaining_digest = EsysHeapWrapper::<tss_sys::TPM2B_DIGEST>::null();
        let return_code = unsafe {
            tss_sys::Esys_GetRandom(
                self.esys_context,
                self.session_handle_one(),
                self.session_handle_two(),
                self.session_handle_three(),
                remaining_bytes.try_into().expect("this should always be bounded by u16"),
                &mut remaining_digest.inner,
            )
        };
        if return_code != 0 {
            return Err(TpmError::TssReturnCode(return_code));
        }
        random_bytes.extend(unsafe { (*remaining_digest.inner).buffer });
        Ok(random_bytes)
    }

    /// Calls `TPM2_GetCapability` attempting to retrieve the `capability` in
    /// the quantity specified by the `property_count`.
    /// See: Trusted Platform Module Library, Part 3: Commands,
    ///      Subsection: 30.2.1
    pub fn get_capability(
        &self,
        capability: &Tpm2Capability,
        property_count: u32,
    ) -> Result<tss_sys::TPMS_CAPABILITY_DATA, TpmError> {
        let mut more_data: tss_sys::TPMI_YES_NO = tss_sys::TPM2_NO as tss_sys::TPMI_YES_NO;
        let mut capability_data = EsysHeapWrapper::<tss_sys::TPMS_CAPABILITY_DATA>::null();
        let return_code = unsafe {
            tss_sys::Esys_GetCapability(
                self.esys_context,
                self.session_handle_one(),
                self.session_handle_two(),
                self.session_handle_three(),
                capability.into(),
                capability_property(&capability),
                property_count,
                &mut more_data,
                &mut capability_data.inner,
            )
        };
        if return_code != 0 {
            return Err(TpmError::TssReturnCode(return_code));
        }
        // Clone the data out of the C heap allocated structure to simplify
        // memory management for the callee.
        let capability_clone = unsafe { (*capability_data.inner).clone() };
        Ok(capability_clone)
    }
}

/// Finalizes and frees the memory associated with the internal `esys_context`.
impl Drop for EsysContext {
    fn drop(&mut self) {
        unsafe {
            tss_sys::Esys_Finalize(&mut self.esys_context);
        };
    }
}
