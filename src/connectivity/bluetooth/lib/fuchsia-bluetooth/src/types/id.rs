// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::{format_err, Error};
use fidl_fuchsia_bluetooth as fidl;
use fuchsia_inspect_contrib::log::WriteInspect;
use std::{fmt, str::FromStr};

/// Valid id strings have only Hex characters (0-9, a-f) and are 16 chars long
/// to match the 64 bit representation of a PeerId.
fn parse_hex_identifier(hex_repr: &str) -> Result<u64, Error> {
    if hex_repr.len() > 16 {
        return Err(format_err!("Id string representation is longer than 16 characters"));
    }
    match u64::from_str_radix(hex_repr, 16) {
        Ok(id) => Ok(id),
        Err(_) => Err(format_err!("Id string representation is not valid hexadecimal")),
    }
}

/// A Fuchsia-generated unique Identifier for a remote Peer that has been observed by the system
/// Identifiers are currently guaranteed to be stable for the duration of system uptime, including
/// if the peer is lost and then re-observed. Identifiers are *not* guaranteed to be stable between
/// reboots.
#[derive(Copy, Clone, Debug, Eq, Hash, PartialEq)]
pub struct PeerId(pub u64);

impl PeerId {
    pub fn random() -> PeerId {
        PeerId(rand::random())
    }
}

impl fmt::Display for PeerId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        // Zero-Pad the output string to be 16 characters to maintain formatting consistency.
        write!(f, "{:016x}", self.0)
    }
}

impl FromStr for PeerId {
    type Err = anyhow::Error;

    fn from_str(src: &str) -> Result<Self, Self::Err> {
        parse_hex_identifier(src).map(|n| PeerId(n))
    }
}

impl From<fidl::PeerId> for PeerId {
    fn from(src: fidl::PeerId) -> PeerId {
        PeerId(src.value)
    }
}

impl Into<fidl::PeerId> for PeerId {
    fn into(self) -> fidl::PeerId {
        fidl::PeerId { value: self.0 }
    }
}

impl WriteInspect for PeerId {
    fn write_inspect(
        &self,
        writer: &fuchsia_inspect::Node,
        key: impl Into<fuchsia_inspect::StringReference>,
    ) {
        writer.record_string(key, self.to_string());
    }
}

/// A Bluetooth Host Adapter id. Uniquely identifiers a bluetooth Host on this system.
/// `HostId` can be converted to/from a FIDL Bluetooth HostId type.
#[derive(Copy, Clone, Debug, Eq, Hash, PartialEq)]
pub struct HostId(pub u64);

impl fmt::Display for HostId {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        // Zero-Pad the output string to be 16 characters to maintain formatting consistency.
        write!(f, "{:016x}", self.0)
    }
}

impl FromStr for HostId {
    type Err = anyhow::Error;

    /// Valid HostId strings have only Hex characters (0-9, a-f) and are 16 chars long
    /// to match the 64 bit representation of a HostId.
    fn from_str(src: &str) -> Result<HostId, Error> {
        parse_hex_identifier(&src).map(|r| HostId(r))
    }
}

impl From<fidl::HostId> for HostId {
    fn from(src: fidl::HostId) -> HostId {
        HostId(src.value)
    }
}

impl Into<fidl::HostId> for HostId {
    fn into(self) -> fidl::HostId {
        fidl::HostId { value: self.0 }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use proptest::prelude::*;

    #[test]
    fn peerid_to_string() {
        let testcases = vec![
            // Lowest possible id.
            (PeerId(0), "0000000000000000"),
            // Normal case with padding.
            (PeerId(1234567890), "00000000499602d2"),
            // Normal case with padding.
            (PeerId(123123777771778888), "01b56c6c6d7db348"),
            // Normal case without padding.
            (PeerId(2000037777717788818), "1bc18fc31e3b0092"),
            // u64 max test.
            (PeerId(std::u64::MAX), "ffffffffffffffff"),
        ];

        for (id, expected) in testcases {
            assert_eq!(expected, id.to_string());
        }
    }

    #[test]
    fn id_to_string() {
        let testcases = vec![
            // Lowest possible id.
            (HostId(0), "0000000000000000"),
            // Normal case with padding.
            (HostId(1234567890), "00000000499602d2"),
            // Normal case with padding.
            (HostId(123123777771778888), "01b56c6c6d7db348"),
            // Normal case without padding.
            (HostId(2000037777717788818), "1bc18fc31e3b0092"),
            // u64 max test.
            (HostId(std::u64::MAX), "ffffffffffffffff"),
        ];

        for (id, expected) in testcases {
            assert_eq!(expected, id.to_string());
        }
    }

    #[test]
    fn peerid_from_string() {
        let testcases = vec![
            // Largest valid id.
            ("ffffffffffffffff", Ok(PeerId(18446744073709551615))),
            // Smallest valid id.
            ("0000000000000000", Ok(PeerId(0))),
            // BT stack wont produce IDs that aren't 16 characters long, but the conversion
            // can handle smaller string ids.
            // In the reverse direction, the string will be padded to 16 characters.
            ("10", Ok(PeerId(16))),
            // Normal case.
            ("fe12ffdda3b89002", Ok(PeerId(18307976762614124546))),
            // String with invalid hex chars (i.e not 0-9, A-F).
            ("klinvalidstr", Err(())),
            // String that is too long to be a PeerId (> 16 chars).
            ("90000111122223333", Err(())),
        ];

        for (input, expected) in testcases {
            assert_eq!(expected, input.parse::<PeerId>().map_err(|_| ()))
        }
    }

    #[test]
    fn id_from_string() {
        let testcases = vec![
            // Largest valid id.
            ("ffffffffffffffff", Ok(HostId(18446744073709551615))),
            // Smallest valid id.
            ("0000000000000000", Ok(HostId(0))),
            // BT stack wont produce IDs that aren't 16 characters long, but the conversion
            // can handle smaller string ids.
            // In the reverse direction, the string will be padded to 16 characters.
            ("10", Ok(HostId(16))),
            // Normal case.
            ("fe12ffdda3b89002", Ok(HostId(18307976762614124546))),
            // String with invalid hex chars (i.e not 0-9, A-F).
            ("klinvalidstr", Err(())),
            // String that is too long to be a Id (> 16 chars).
            ("90000111122223333", Err(())),
        ];

        for (input, expected) in testcases {
            assert_eq!(expected, input.parse::<HostId>().map_err(|_| ()))
        }
    }

    proptest! {
        #[test]
        fn peerid_string_roundtrip(n in prop::num::u64::ANY) {
            let peer_id = PeerId(n);
            assert_eq!(Ok(peer_id), peer_id.to_string().parse::<PeerId>().map_err(|_| ()));
        }

        #[test]
        fn peerid_fidl_roundtrip(n in prop::num::u64::ANY) {
            let peer_id = PeerId(n);
            let fidl_id: fidl::PeerId = peer_id.into();
            assert_eq!(peer_id, PeerId::from(fidl_id));
        }

        #[test]
        fn peerid_into_fidl(n in prop::num::u64::ANY) {
            let peer_id = PeerId(n);
            let fidl_p_id: fidl::PeerId = peer_id.into();
            assert_eq!(n, fidl_p_id.value);
        }

        #[test]
        fn id_into_fidl(n in prop::num::u64::ANY) {
            let id = HostId(n);
            let fidl_id: fidl::HostId = id.into();
            assert_eq!(n, fidl_id.value);
        }

        #[test]
        fn peer_id_from_fidl(n in prop::num::u64::ANY) {
            let fidl_p_id = fidl::PeerId { value: n };
            let peer_id: PeerId = fidl_p_id.into();
            assert_eq!(PeerId(n), peer_id);
        }

        #[test]
        fn id_from_fidl(n in prop::num::u64::ANY) {
            let fidl_id = fidl::HostId { value: n };
            let id: HostId = fidl_id.into();
            assert_eq!(HostId(n), id);
        }
    }
}
