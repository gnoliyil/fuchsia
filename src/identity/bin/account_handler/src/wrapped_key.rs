// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! This file contains structs and helpers for wrapping/unwrapping an encryption
//! key using AES-256-GCM.

use {
    aes_gcm::{
        aead::{
            generic_array::{
                typenum::{U12, U32},
                GenericArray,
            },
            Aead, NewAead,
        },
        Aes256Gcm, Nonce,
    },
    fidl_fuchsia_identity_account::Error as ApiError,
    lazy_static::lazy_static,
    rand::{thread_rng, Rng},
    serde::{Deserialize, Serialize},
    tracing::{error, warn},
};

lazy_static! {
    static ref NULL_KEY: GenericArray<u8, U32> = *GenericArray::from_slice(&[0_u8; 32]);
}

// Generates a random 96-bit array and returns it as a generic array of 12 u8s.
fn make_random_96_bit_generic_array() -> GenericArray<u8, U12> {
    let mut bits = [0_u8; 12];
    thread_rng().fill(&mut bits[..]);
    *GenericArray::from_slice(&bits)
}

#[derive(Debug, PartialEq, Clone, Deserialize, Serialize)]
// WrappedKey holds the ciphertext and nonce of an AES-256-GCM encryption for
// some plaintext, where the nonce is a 96-bit random number.
//
// The ciphertext was generated by encrypting some plaintext with a key derived
// from the authenticator prekey material:
//
// ```
// cipher = new_cipher(prekey)
// (ciphertext+tag) = cipher.encrypt(plaintext, nonce)
// plaintext = cipher.decrypt((ciphertext+tag), nonce)
// ```
//
// It can be decrypted by an Aes256Gcm object constructed from that same key.
//
// Since the plaintext is 256 and the tag is 128 bits, the ciphertext vector
// here will be 384 bytes.
pub struct WrappedKey {
    // The underlying data, wrapped via AES-256-GCM with some key, not stored here.
    pub ciphertext_and_tag: Vec<u8>,

    // A random 96-bit number.
    pub nonce: [u8; 12],
}
impl WrappedKey {
    fn make_from(cipher: &Aes256Gcm, plaintext: &GenericArray<u8, U32>) -> Option<WrappedKey> {
        let nonce: Nonce<U12> = *Nonce::from_slice(&make_random_96_bit_generic_array());
        let ciphertext = cipher.encrypt(&nonce, &**plaintext).ok()?;
        Some(WrappedKey { ciphertext_and_tag: ciphertext.to_vec(), nonce: nonce.into() })
    }
    fn unwrap_key(&self, cipher: &mut Aes256Gcm) -> Result<[u8; 32], ApiError> {
        cipher
            .decrypt((&self.nonce).into(), self.ciphertext_and_tag.as_ref())
            .map_err(|_e| {
                warn!(
                    "Failed to unwrap AES-256-GCM ciphertext. Was the wrong prekey material \
                    provided?"
                );
                ApiError::FailedAuthentication
            })?
            .try_into()
            .map_err(|_| {
                warn!(
                    "Unwrapped AES-256-GCM ciphertext given prekey material, but the resultant \
                    plaintext was of the wrong length -- expected [u8; 32]."
                );
                ApiError::Internal
            })
    }
}

#[derive(Debug, PartialEq, Clone, Deserialize, Serialize)]
// WrappedKeySet holds:
//
// - a disk key (in this case, the encryption key for some storage volume)
//   wrapped via AES-256-GCM with some prekey material.
//
// - a null key (i.e. 256 bytes of all zeroes) wrapped via AES-256-GCM with that
//   same prekey material. We store this in order to attempt to decode it first,
//   in order to know whether or not a key used to attempt decryption is valid
//   without needing to try to use it to unlock a volume.
//
// Both keys are stored in (ciphertext+tag, nonce) form (see WrappedKey), and
// both are wrapped with the same prekey material.
pub struct WrappedKeySet {
    pub wrapped_disk_key: WrappedKey,
    pub wrapped_null_key: WrappedKey,
}

impl WrappedKeySet {
    // First unwraps the null key and ensures that it unwraps to the expected
    // NULL_KEY value. Then unwraps the disk key and returns it.
    //
    // If the call to self.wrapped_null_key.unwrap_key() does not produce
    // NULL_KEY, it means that the wrong prekey_material was supplied, and we
    // return ApiError::FailedPrecondition.
    //
    // If the internal unwrap_key calls fail, we return ApiError::Internal.
    pub fn unwrap_disk_key(&self, prekey_material: &[u8]) -> Result<[u8; 32], ApiError> {
        let mut cipher = Aes256Gcm::new(GenericArray::from_slice(prekey_material));

        match self.wrapped_null_key.unwrap_key(&mut cipher) {
            Err(ApiError::FailedAuthentication) => {
                warn!(
                    "Failed to unwrap the encrypted null key using AES-256-GCM. This probably \
                    means that the prekey material did not match that supplied during creation."
                );
                Err(ApiError::FailedAuthentication)
            }
            Err(e) => Err(e),
            Ok(decrypted_null_key) if decrypted_null_key == NULL_KEY.as_slice() => {
                // If the unwrapped encrypted null key is equal to the NULL_KEY,
                // then the provided prekey material is correct -- proceed to unwrap the disk key.
                self.wrapped_disk_key.unwrap_key(&mut cipher)
            }
            Ok(_) => {
                warn!(
                    "Unwrapped the encrypted null key using AES-256-GCM, but the resultant value \
                    was not actually the null key. This probably means that the prekey material \
                    did not match that supplied during creation."
                );
                Err(ApiError::FailedAuthentication)
            }
        }
    }
}

// Given prekey material, this function:
// - instantiates a new Aes256Gcm cipher,
// - generates a new random volume encryption key, and
// - wraps both the volume // encryption key and a 256-bit null key with two
//   different random nonces.
//
// Both the wrapped volume encryption key and the wrapped null key are returned
// so that they can be stored in the EnrollmentState struct above.
pub fn produce_wrapped_keys(
    prekey_material: Vec<u8>,
    disk_key: &GenericArray<u8, U32>,
) -> Result<WrappedKeySet, ApiError> {
    let cipher = Aes256Gcm::new(GenericArray::from_slice(&prekey_material));

    let wrapped_disk_key = WrappedKey::make_from(&cipher, disk_key).ok_or_else(|| {
        error!("Could not compute E(volume_encryption_key).");
        ApiError::Internal
    })?;

    let wrapped_null_key = WrappedKey::make_from(&cipher, &NULL_KEY).ok_or_else(|| {
        error!("Could not compute E(null_key).");
        ApiError::Internal
    })?;

    Ok(WrappedKeySet { wrapped_disk_key, wrapped_null_key })
}

// Generates a random 256-bit array and returns it as a generic array of 32 u8s.
pub fn make_random_256_bit_generic_array() -> GenericArray<u8, U32> {
    let mut bits = [0_u8; 32];
    thread_rng().fill(&mut bits[..]);
    *GenericArray::from_slice(&bits)
}

#[cfg(test)]
mod tests {
    use super::*;
    use assert_matches::assert_matches;

    fn make_random_256_bit_array() -> [u8; 32] {
        let mut bits = [0_u8; 32];
        thread_rng().fill(&mut bits[..]);
        bits
    }

    #[test]
    fn test_wrapped_keys_are_different() {
        let prekey_material = make_random_256_bit_array().to_vec();
        let disk_key = make_random_256_bit_generic_array();
        let wk = assert_matches!(produce_wrapped_keys(prekey_material, &disk_key), Ok(m) => m);
        assert_ne!(wk.wrapped_disk_key, wk.wrapped_null_key);
    }

    #[test]
    fn test_wrapped_keys_have_correct_length() {
        let prekey_material = make_random_256_bit_array().to_vec();
        let disk_key = make_random_256_bit_generic_array();
        let wk = assert_matches!(produce_wrapped_keys(prekey_material, &disk_key), Ok(m) => m);
        // Expected to be more than 32 because it includes the tag.
        assert_eq!(wk.wrapped_disk_key.ciphertext_and_tag.len(), 48);
        assert_eq!(wk.wrapped_null_key.ciphertext_and_tag.len(), 48);
    }

    #[test]
    fn test_wrapped_keys_not_hermetic() {
        // Should have different output with the same input.
        let prekey_material = make_random_256_bit_array().to_vec();
        let disk_key = make_random_256_bit_generic_array();
        let wrapped_keys_1 =
            assert_matches!(produce_wrapped_keys(prekey_material.clone(), &disk_key), Ok(m) => m);
        let wrapped_keys_2 =
            assert_matches!(produce_wrapped_keys(prekey_material, &disk_key), Ok(m) => m);
        assert_ne!(wrapped_keys_1.wrapped_disk_key, wrapped_keys_2.wrapped_disk_key,);
        assert_ne!(wrapped_keys_1.wrapped_null_key, wrapped_keys_2.wrapped_null_key,);
    }

    #[test]
    fn test_wrapped_keys_null_key_decrypts_to_null() {
        let prekey_material = make_random_256_bit_array().to_vec();
        let disk_key = make_random_256_bit_generic_array();
        let wk =
            assert_matches!(produce_wrapped_keys(prekey_material.clone(), &disk_key), Ok(m) => m);

        let cipher = Aes256Gcm::new(GenericArray::from_slice(&prekey_material));

        let actual_plaintext = assert_matches!(
            cipher.decrypt(
                (&wk.wrapped_null_key.nonce).into(),
                wk.wrapped_null_key.ciphertext_and_tag.as_ref(),
            ),
            Ok(p) => p
        );

        assert_eq!(actual_plaintext, NULL_KEY.to_vec());
    }

    #[test]
    fn test_wrapped_keys_volume_key_can_be_decrypted() {
        let prekey_material = make_random_256_bit_array().to_vec();
        let disk_key = make_random_256_bit_generic_array();
        let wk =
            assert_matches!(produce_wrapped_keys(prekey_material.clone(), &disk_key), Ok(m) => m);

        let cipher = Aes256Gcm::new(GenericArray::from_slice(&prekey_material));

        assert_matches!(
            cipher.decrypt(
                (&wk.wrapped_disk_key.nonce).into(),
                wk.wrapped_disk_key.ciphertext_and_tag.as_ref(),
            ),
            Ok(_)
        );
    }

    #[test]
    fn test_wrapped_keys_unwrap_helper_fn() {
        let prekey_material = make_random_256_bit_array().to_vec();
        let disk_key = make_random_256_bit_generic_array();
        let wk =
            assert_matches!(produce_wrapped_keys(prekey_material.clone(), &disk_key), Ok(m) => m);

        // That this returns proves that:
        //  - the codepath to decrypt_wrapped_key(.., wrapped_null_key) succeeds,
        //  - the decrypted null key is null, and
        //  - the codepath to decrypt_wrapped_key(.., wrapped_disk_key) succeeds.
        let _actual_disk_key: [u8; 32] = assert_matches!(
            wk.unwrap_disk_key(&prekey_material),
            Ok(plaintext) => plaintext
        );
    }

    #[test]
    fn test_wrapped_keys_wrong_prekey() {
        let prekey_material = make_random_256_bit_array().to_vec();
        let disk_key = make_random_256_bit_generic_array();
        let wk = assert_matches!(produce_wrapped_keys(prekey_material, &disk_key), Ok(m) => m);

        let other_prekey_material = make_random_256_bit_array().to_vec();
        assert_matches!(
            wk.unwrap_disk_key(&other_prekey_material),
            Err(ApiError::FailedAuthentication)
        );
    }
}
