// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! This module defines the `Uuid` type which represents a 128-bit Bluetooth UUID. It provides
//! convenience functions to support 16-bit, 32-bit, and 128-bit canonical formats as well as
//! string representation. It can be converted to/from a fuchsia.bluetooth.Uuid FIDL type.

use fidl_fuchsia_bluetooth as fidl;
use fidl_fuchsia_bluetooth_bredr as fidlbredr;
use serde::{Deserialize, Serialize};
use std::convert::{TryFrom, TryInto};
use std::str::FromStr;
use uuid;

use crate::error::Error;
use crate::inspect::ToProperty;

#[derive(Clone, Debug, Deserialize, PartialEq, Serialize)]
pub struct Uuid(uuid::Uuid);

/// Last eight bytes of the BASE UUID, in big-endian order, for comparision.
const BASE_UUID_FINAL_EIGHT_BYTES: [u8; 8] = [0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB];

impl Uuid {
    /// Create a new Uuid from a little-endian array of 16 bytes.
    pub const fn from_bytes(bytes_little_endian: uuid::Bytes) -> Uuid {
        let u = u128::from_le_bytes(bytes_little_endian);
        Uuid(uuid::Uuid::from_u128(u))
    }

    pub const fn new16(value: u16) -> Uuid {
        Uuid::new32(value as u32)
    }

    pub const fn new32(value: u32) -> Uuid {
        // Note: It is safe to unwrap the result here a `from_fields` only errors if the final
        // slice length != 8, and here we are enforcing a constant value of length 8.
        Uuid(uuid::Uuid::from_fields(value, 0x0000, 0x1000, &BASE_UUID_FINAL_EIGHT_BYTES))
    }

    pub fn to_string(&self) -> String {
        self.0.as_hyphenated().to_string()
    }
}

impl TryFrom<Uuid> for u32 {
    type Error = Error;

    fn try_from(u: Uuid) -> Result<u32, <u32 as TryFrom<Uuid>>::Error> {
        let (first, second, third, final_bytes) = u.0.as_fields();
        if second != 0x0000 || third != 0x1000 || final_bytes != &BASE_UUID_FINAL_EIGHT_BYTES {
            return Err(Error::conversion("not derived from the base UUID"));
        }
        Ok(first)
    }
}

impl TryFrom<Uuid> for u16 {
    type Error = Error;

    fn try_from(u: Uuid) -> Result<u16, <u16 as TryFrom<Uuid>>::Error> {
        let x: u32 = u.try_into()?;
        x.try_into().map_err(|_e| Error::conversion("not a 16-bit UUID"))
    }
}

impl From<&fidl::Uuid> for Uuid {
    fn from(src: &fidl::Uuid) -> Uuid {
        Uuid::from_bytes(src.value)
    }
}

impl From<fidl::Uuid> for Uuid {
    fn from(src: fidl::Uuid) -> Uuid {
        Uuid::from(&src)
    }
}

impl From<&Uuid> for fidl::Uuid {
    fn from(src: &Uuid) -> fidl::Uuid {
        let mut bytes = src.0.as_bytes().clone();
        bytes.reverse();
        fidl::Uuid { value: bytes }
    }
}

impl From<Uuid> for fidl::Uuid {
    fn from(src: Uuid) -> fidl::Uuid {
        fidl::Uuid::from(&src)
    }
}

impl From<uuid::Uuid> for Uuid {
    fn from(src: uuid::Uuid) -> Uuid {
        Uuid(src)
    }
}

impl From<Uuid> for uuid::Uuid {
    fn from(src: Uuid) -> uuid::Uuid {
        src.0
    }
}

impl TryFrom<Uuid> for fidlbredr::ServiceClassProfileIdentifier {
    type Error = Error;

    fn try_from(value: Uuid) -> Result<Self, Self::Error> {
        let short: u16 = value.try_into()?;
        Self::from_primitive(short)
            .ok_or(Error::conversion(format!("unknown ServiceClassProfileIdentifier: {short}")))
    }
}

impl From<fidlbredr::ServiceClassProfileIdentifier> for Uuid {
    fn from(src: fidlbredr::ServiceClassProfileIdentifier) -> Self {
        Uuid::new16(src.into_primitive())
    }
}

impl From<Uuid> for fidlbredr::DataElement {
    fn from(src: Uuid) -> Self {
        fidlbredr::DataElement::Uuid(src.into())
    }
}

impl FromStr for Uuid {
    type Err = Error;

    fn from_str(s: &str) -> Result<Uuid, Self::Err> {
        uuid::Uuid::parse_str(s).map(|uuid| Uuid(uuid)).map_err(Error::external)
    }
}

impl ToProperty for Uuid {
    type PropertyType = String;
    fn to_property(&self) -> Self::PropertyType {
        self.to_string()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use proptest::prelude::*;

    #[test]
    fn uuid16_to_string() {
        let uuid = Uuid::new16(0x180d);
        assert_eq!("0000180d-0000-1000-8000-00805f9b34fb", uuid.to_string());
    }

    #[test]
    fn uuid32_to_string() {
        let uuid = Uuid::new32(0xAABBCCDD);
        assert_eq!("aabbccdd-0000-1000-8000-00805f9b34fb", uuid.to_string());
    }

    proptest! {
        #[test]
        fn all_uuid32_valid(n in prop::num::u32::ANY) {
            // Ensure that the for all u32, we do not panic and produce a Uuid
            // with the correct suffix
            let uuid = Uuid::new32(n);
            let string = uuid.to_string();
            assert_eq!("-0000-1000-8000-00805f9b34fb", &(string[8..]));
            let back: u32 = uuid.try_into().expect("can to back to u32");
            assert_eq!(back, n);
        }
    }

    proptest! {
        #[test]
        fn all_uuid16_valid(n in prop::num::u16::ANY) {
            // Ensure that the for all u16, we do not panic and produce a Uuid
            // with the correct suffix
            let uuid = Uuid::new16(n);
            let string = uuid.to_string();
            assert_eq!("-0000-1000-8000-00805f9b34fb", &(string[8..]));
            assert_eq!("00", &(string[0..2]));
            let back: u16 = uuid.try_into().expect("can to back to u16");
            assert_eq!(back, n);
        }
    }

    proptest! {
        #[test]
        fn parser_roundtrip(n in prop::num::u32::ANY) {
            let uuid = Uuid::new32(n);
            let string = uuid.to_string();
            let parsed = string.parse::<Uuid>();
            assert_eq!(Ok(uuid), parsed.map_err(|e| format!("{:?}", e)));
        }
    }

    #[test]
    fn uuid128_to_string() {
        let uuid = Uuid::from_bytes([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15]);
        assert_eq!("0f0e0d0c-0b0a-0908-0706-050403020100", uuid.to_string());
    }

    #[test]
    fn uuid_from_fidl() {
        let uuid = fidl::Uuid { value: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15] };
        let uuid: Uuid = uuid.into();
        assert_eq!("0f0e0d0c-0b0a-0908-0706-050403020100", uuid.to_string());
    }

    #[test]
    fn uuid_into_fidl() {
        let uuid = Uuid::from_bytes([0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15]);
        let uuid: fidl::Uuid = uuid.into();
        let expected = fidl::Uuid { value: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15] };
        assert_eq!(expected, uuid);
    }
}
