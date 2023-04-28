// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::errors::{
        BlobIdFromSliceError, BlobIdParseError, CupMissingField, ResolutionContextError,
    },
    fidl_fuchsia_pkg as fidl,
    proptest_derive::Arbitrary,
    serde::{Deserialize, Serialize},
    std::{convert::TryFrom, fmt, str},
    typed_builder::TypedBuilder,
};

pub(crate) const BLOB_ID_SIZE: usize = 32;

/// Convenience wrapper type for the autogenerated FIDL `BlobId`.
#[derive(Copy, Clone, Eq, PartialEq, Ord, PartialOrd, Hash, Serialize, Deserialize, Arbitrary)]
pub struct BlobId(#[serde(with = "hex_serde")] [u8; BLOB_ID_SIZE]);

impl BlobId {
    /// Parse a `BlobId` from a string containing 32 lower-case hex encoded bytes.
    ///
    /// # Examples
    /// ```
    /// let s = "00112233445566778899aabbccddeeffffeeddccbbaa99887766554433221100";
    /// assert_eq!(
    ///     BlobId::parse(s),
    ///     s.parse()
    /// );
    /// ```
    pub fn parse(s: &str) -> Result<Self, BlobIdParseError> {
        s.parse()
    }
    /// Obtain a slice of bytes representing the `BlobId`.
    pub fn as_bytes(&self) -> &[u8] {
        &self.0[..]
    }
}

impl str::FromStr for BlobId {
    type Err = BlobIdParseError;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        let bytes = hex::decode(s)?;
        if bytes.len() != BLOB_ID_SIZE {
            return Err(BlobIdParseError::InvalidLength(bytes.len()));
        }
        if s.chars().any(|c| c.is_uppercase()) {
            return Err(BlobIdParseError::CannotContainUppercase);
        }
        let mut res: [u8; BLOB_ID_SIZE] = [0; BLOB_ID_SIZE];
        res.copy_from_slice(&bytes[..]);
        Ok(Self(res))
    }
}

impl From<[u8; BLOB_ID_SIZE]> for BlobId {
    fn from(bytes: [u8; BLOB_ID_SIZE]) -> Self {
        Self(bytes)
    }
}

impl From<fidl::BlobId> for BlobId {
    fn from(id: fidl::BlobId) -> Self {
        Self(id.merkle_root)
    }
}

impl From<BlobId> for fidl::BlobId {
    fn from(id: BlobId) -> Self {
        Self { merkle_root: id.0 }
    }
}

impl From<fuchsia_hash::Hash> for BlobId {
    fn from(hash: fuchsia_hash::Hash) -> Self {
        Self(hash.into())
    }
}

impl TryFrom<&[u8]> for BlobId {
    type Error = BlobIdFromSliceError;

    fn try_from(bytes: &[u8]) -> Result<Self, Self::Error> {
        Ok(Self(bytes.try_into().map_err(|_| Self::Error::InvalidLength(bytes.len()))?))
    }
}

impl From<BlobId> for fuchsia_hash::Hash {
    fn from(id: BlobId) -> Self {
        id.0.into()
    }
}

impl fmt::Display for BlobId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&hex::encode(self.0))
    }
}

impl fmt::Debug for BlobId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.to_string())
    }
}

/// Convenience wrapper type for the autogenerated FIDL `BlobInfo`.
#[derive(Copy, Clone, Debug, Eq, PartialEq, Ord, PartialOrd, Hash, Arbitrary)]
pub struct BlobInfo {
    pub blob_id: BlobId,
    pub length: u64,
}

impl BlobInfo {
    pub fn new(blob_id: BlobId, length: u64) -> Self {
        BlobInfo { blob_id: blob_id, length: length }
    }
}

impl From<fidl::BlobInfo> for BlobInfo {
    fn from(info: fidl::BlobInfo) -> Self {
        BlobInfo { blob_id: info.blob_id.into(), length: info.length }
    }
}

impl From<BlobInfo> for fidl::BlobInfo {
    fn from(info: BlobInfo) -> Self {
        Self { blob_id: info.blob_id.into(), length: info.length }
    }
}

/// Convenience wrapper type for the autogenerated FIDL `ResolutionContext`.
#[derive(Clone, Debug, Eq, PartialEq, Ord, PartialOrd, Hash)]
pub struct ResolutionContext {
    blob_id: Option<BlobId>,
}

impl ResolutionContext {
    /// Creates an empty resolution context.
    pub fn new() -> Self {
        Self { blob_id: None }
    }

    /// The ResolutionContext's optional blob id.
    pub fn blob_id(&self) -> Option<&BlobId> {
        self.blob_id.as_ref()
    }
}

impl From<BlobId> for ResolutionContext {
    fn from(blob_id: BlobId) -> Self {
        Self { blob_id: Some(blob_id) }
    }
}

impl TryFrom<&[u8]> for ResolutionContext {
    type Error = ResolutionContextError;

    fn try_from(bytes: &[u8]) -> Result<Self, Self::Error> {
        if bytes.is_empty() {
            Ok(Self { blob_id: None })
        } else {
            Ok(Self { blob_id: Some(bytes.try_into().map_err(Self::Error::InvalidBlobId)?) })
        }
    }
}

impl TryFrom<&fidl::ResolutionContext> for ResolutionContext {
    type Error = ResolutionContextError;

    fn try_from(context: &fidl::ResolutionContext) -> Result<Self, Self::Error> {
        Self::try_from(context.bytes.as_slice())
    }
}

impl From<ResolutionContext> for Vec<u8> {
    fn from(context: ResolutionContext) -> Self {
        match context.blob_id {
            Some(blob_id) => blob_id.as_bytes().to_vec(),
            None => vec![],
        }
    }
}

impl From<ResolutionContext> for fidl::ResolutionContext {
    fn from(context: ResolutionContext) -> Self {
        Self { bytes: context.into() }
    }
}

mod hex_serde {
    use {super::BLOB_ID_SIZE, hex::FromHex, serde::Deserialize};

    pub fn serialize<S>(bytes: &[u8; BLOB_ID_SIZE], serializer: S) -> Result<S::Ok, S::Error>
    where
        S: serde::Serializer,
    {
        let s = hex::encode(bytes);
        serializer.serialize_str(&s)
    }

    pub fn deserialize<'de, D>(deserializer: D) -> Result<[u8; BLOB_ID_SIZE], D::Error>
    where
        D: serde::Deserializer<'de>,
    {
        let value = String::deserialize(deserializer)?;
        <[u8; BLOB_ID_SIZE]>::from_hex(value.as_bytes())
            .map_err(|e| serde::de::Error::custom(format!("bad hex value: {:?}: {}", value, e)))
    }
}

#[derive(Clone, Debug, Eq, PartialEq, TypedBuilder)]
pub struct CupData {
    #[builder(default, setter(into))]
    pub request: Vec<u8>,
    #[builder(default, setter(into))]
    pub key_id: u64,
    #[builder(default, setter(into))]
    pub nonce: [u8; 32],
    #[builder(default, setter(into))]
    pub response: Vec<u8>,
    #[builder(default, setter(into))]
    pub signature: Vec<u8>,
}

impl From<CupData> for fidl::CupData {
    fn from(c: CupData) -> Self {
        fidl::CupData {
            request: Some(c.request),
            key_id: Some(c.key_id),
            nonce: Some(c.nonce),
            response: Some(c.response),
            signature: Some(c.signature),
            ..Default::default()
        }
    }
}

impl TryFrom<fidl::CupData> for CupData {
    type Error = CupMissingField;
    fn try_from(c: fidl::CupData) -> Result<Self, Self::Error> {
        Ok(CupData::builder()
            .request(c.request.ok_or(CupMissingField::Request)?)
            .response(c.response.ok_or(CupMissingField::Response)?)
            .key_id(c.key_id.ok_or(CupMissingField::KeyId)?)
            .nonce(c.nonce.ok_or(CupMissingField::Nonce)?)
            .signature(c.signature.ok_or(CupMissingField::Signature)?)
            .build())
    }
}

#[cfg(test)]
mod test_blob_id {
    use {super::*, assert_matches::assert_matches, proptest::prelude::*};

    prop_compose! {
        fn invalid_hex_char()(c in "[[:ascii:]&&[^0-9a-fA-F]]") -> char {
            assert_eq!(c.len(), 1);
            c.chars().next().unwrap()
        }
    }

    proptest! {
        #![proptest_config(ProptestConfig{
            // Disable persistence to avoid the warning for not running in the
            // source code directory (since we're running on a Fuchsia target)
            failure_persistence: None,
            .. ProptestConfig::default()
        })]

        #[test]
        fn parse_is_inverse_of_display(ref id in "[0-9a-f]{64}") {
            prop_assert_eq!(
                id,
                &format!("{}", id.parse::<BlobId>().unwrap())
            );
        }

        #[test]
        fn parse_is_inverse_of_debug(ref id in "[0-9a-f]{64}") {
            prop_assert_eq!(
                id,
                &format!("{:?}", id.parse::<BlobId>().unwrap())
            );
        }

        #[test]
        fn parse_rejects_uppercase(ref id in "[0-9A-F]{64}") {
            prop_assert_eq!(
                id.parse::<BlobId>(),
                Err(BlobIdParseError::CannotContainUppercase)
            );
        }

        #[test]
        fn parse_rejects_unexpected_characters(mut id in "[0-9a-f]{64}", c in invalid_hex_char(), index in 0usize..63) {
            id.remove(index);
            id.insert(index, c);
            prop_assert_eq!(
                id.parse::<BlobId>(),
                Err(BlobIdParseError::FromHexError(
                    hex::FromHexError::InvalidHexCharacter { c: c, index: index }
                ))
            );
        }

        #[test]
        fn parse_expects_even_sized_strings(ref id in "[0-9a-f]([0-9a-f]{2})*") {
            prop_assert_eq!(
                id.parse::<BlobId>(),
                Err(BlobIdParseError::FromHexError(hex::FromHexError::OddLength))
            );
        }

        #[test]
        fn parse_expects_precise_count_of_bytes(ref id in "([0-9a-f]{2})*") {
            prop_assume!(id.len() != BLOB_ID_SIZE * 2);
            prop_assert_eq!(
                id.parse::<BlobId>(),
                Err(BlobIdParseError::InvalidLength(id.len() / 2))
            );
        }

        #[test]
        fn fidl_conversions_are_inverses(id: BlobId) {
            let temp : fidl::BlobId = id.into();
            prop_assert_eq!(
                id,
                BlobId::from(temp)
            );
        }
    }

    #[test]
    fn try_from_slice_rejects_invalid_length() {
        assert_matches!(
            BlobId::try_from([0u8; BLOB_ID_SIZE - 1].as_slice()),
            Err(BlobIdFromSliceError::InvalidLength(31))
        );
        assert_matches!(
            BlobId::try_from([0u8; BLOB_ID_SIZE + 1].as_slice()),
            Err(BlobIdFromSliceError::InvalidLength(33))
        );
    }

    #[test]
    fn try_from_slice_succeeds() {
        let bytes = [1u8; 32];
        assert_eq!(BlobId::try_from(bytes.as_slice()).unwrap().as_bytes(), bytes.as_slice());
    }
}

#[cfg(test)]
mod test_resolution_context {
    use {super::*, assert_matches::assert_matches};

    #[test]
    fn try_from_slice_succeeds() {
        assert_eq!(
            ResolutionContext::try_from([].as_slice()).unwrap(),
            ResolutionContext { blob_id: None }
        );

        assert_eq!(
            ResolutionContext::try_from([1u8; 32].as_slice()).unwrap(),
            ResolutionContext { blob_id: Some([1u8; 32].into()) }
        );
    }

    #[test]
    fn try_from_slice_fails() {
        assert_matches!(
            ResolutionContext::try_from([1u8].as_slice()),
            Err(ResolutionContextError::InvalidBlobId(_))
        );
    }

    #[test]
    fn into_vec() {
        assert_eq!(Vec::from(ResolutionContext::new()), Vec::<u8>::new());

        assert_eq!(Vec::from(ResolutionContext::from(BlobId::from([1u8; 32]))), vec![1u8; 32]);
    }

    #[test]
    fn try_from_fidl_succeeds() {
        assert_eq!(
            ResolutionContext::try_from(&fidl::ResolutionContext { bytes: vec![] }).unwrap(),
            ResolutionContext { blob_id: None }
        );

        assert_eq!(
            ResolutionContext::try_from(&fidl::ResolutionContext { bytes: vec![1u8; 32] }).unwrap(),
            ResolutionContext { blob_id: Some([1u8; 32].into()) }
        );
    }

    #[test]
    fn try_from_fidl_fails() {
        assert_matches!(
            ResolutionContext::try_from(&fidl::ResolutionContext { bytes: vec![1u8; 1] }),
            Err(ResolutionContextError::InvalidBlobId(_))
        );
    }

    #[test]
    fn into_fidl() {
        assert_eq!(
            fidl::ResolutionContext::from(ResolutionContext::new()),
            fidl::ResolutionContext { bytes: vec![] }
        );

        assert_eq!(
            fidl::ResolutionContext::from(ResolutionContext::from(BlobId::from([1u8; 32]))),
            fidl::ResolutionContext { bytes: vec![1u8; 32] }
        );
    }
}
