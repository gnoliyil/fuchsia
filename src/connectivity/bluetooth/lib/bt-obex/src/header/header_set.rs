// Copyright 2023 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use packet_encoding::{Decodable, Encodable};

use crate::error::{Error, PacketError};
use crate::header::{Header, HeaderIdentifier};

#[derive(Clone, Debug, PartialEq)]
pub struct HeaderSet {
    // TODO(fxbug.dev/125070): The ordering of headers matters. Make this more efficient by
    // possibly storing Headers in a Vec<T> and maintaining a map of HeaderIdentifier -> index to
    // improve the time complexity of lookups and updates.
    // See OBEX 1.5 Section 3.4 for the ordering requirements for each Operation.
    headers: Vec<(HeaderIdentifier, Header)>,
}

impl HeaderSet {
    pub fn new() -> Self {
        Self { headers: Vec::new() }
    }

    #[cfg(test)]
    pub fn from_headers(headers: Vec<Header>) -> Result<Self, Error> {
        let mut set = Self::new();
        for header in headers {
            set.add(header)?;
        }
        Ok(set)
    }

    pub fn contains_header(&self, id: &HeaderIdentifier) -> bool {
        self.headers.iter().find(|(header_id, _)| header_id == id).is_some()
    }

    pub fn add(&mut self, header: Header) -> Result<(), Error> {
        let id = header.identifier();
        if self.contains_header(&id) {
            return Err(Error::Duplicate(id));
        }

        // TODO(fxbug.dev/125070): Store `header` in the spec-defined order. For now, it is stored
        // in the order it is added.
        self.headers.push((id, header));
        Ok(())
    }
}

impl Encodable for HeaderSet {
    type Error = PacketError;

    fn encoded_len(&self) -> usize {
        self.headers.iter().map(|(_, h)| h.encoded_len()).sum()
    }

    fn encode(&self, buf: &mut [u8]) -> Result<(), Self::Error> {
        if buf.len() < self.encoded_len() {
            return Err(PacketError::BufferTooSmall);
        }
        let mut start_idx = 0;
        for (_, header) in &self.headers {
            header.encode(&mut buf[start_idx..])?;
            start_idx += header.encoded_len();
        }

        Ok(())
    }
}

impl Decodable for HeaderSet {
    type Error = PacketError;

    fn decode(buf: &[u8]) -> Result<Self, Self::Error> {
        let mut headers = Self::new();
        let mut start_idx = 0;
        while start_idx < buf.len() {
            let header = Header::decode(&buf[start_idx..])?;
            start_idx += header.encoded_len();
            headers.add(header).map_err(|e| PacketError::data(format!("{e:?}")))?;
        }
        Ok(headers)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    use assert_matches::assert_matches;

    #[fuchsia::test]
    fn add_duplicate_header_is_error() {
        let mut headers = HeaderSet::new();
        headers.add(Header::Count(123)).expect("can add header");
        assert!(headers.contains_header(&HeaderIdentifier::Count));
        assert_matches!(headers.add(Header::Count(100)), Err(Error::Duplicate(_)));
    }

    #[fuchsia::test]
    fn encode_header_set() {
        let headers = HeaderSet::from_headers(vec![
            Header::ConnectionId(1),
            Header::EndOfBody(vec![1, 2, 3]),
        ])
        .expect("can build header set");

        // The total length should be the sum of the lengths of each Header in the set.
        assert_eq!(headers.encoded_len(), 11);
        let mut buf = vec![0; headers.encoded_len()];
        headers.encode(&mut buf[..]).expect("can encode headers");
        // TODO(fxbug.dev/125070): Potentially update this check when `HeaderSet` enforces ordering
        // during encoding.
        let expected_buf = [0xcb, 0x00, 0x00, 0x00, 0x01, 0x49, 0x00, 0x06, 0x01, 0x02, 0x03];
        assert_eq!(buf, expected_buf);
    }

    #[fuchsia::test]
    fn decode_header_set() {
        let buf = [
            0x05, 0x00, 0x09, 0x00, 0x68, 0x00, 0x65, 0x00,
            0x00, // Description = "he" (String)
            0xd6, 0x00, 0x00, 0x00, 0x05, // Permissions = 5 (u32)
            0x97, 0x01, // SRM = 1 (u8)
        ];
        let headers = HeaderSet::decode(&buf[..]).expect("can decode into headers");
        let expected_body = Header::Description("he".into());
        let expected_permissions = Header::Permissions(5);
        let expected_srm = Header::SingleResponseMode(1);
        let expected_headers =
            HeaderSet::from_headers(vec![expected_body, expected_permissions, expected_srm])
                .unwrap();
        assert_eq!(headers, expected_headers);
    }

    #[fuchsia::test]
    fn decode_partial_header_set_error() {
        // Decoding should fail if one of the headers is invalidly formatted.
        let buf = [
            0xd6, 0x00, 0x00, 0x00, 0x09, // Permissions = 9 (u32)
            0x97, 0x01, // SRM = 1 (u8)
            0xc4, 0x00, // Time4Byte but missing remaining bytes.
        ];
        let headers = HeaderSet::decode(&buf[..]);
        assert_matches!(headers, Err(PacketError::BufferTooSmall));
    }
}
