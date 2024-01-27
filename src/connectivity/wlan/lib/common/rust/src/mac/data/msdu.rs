// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        big_endian::BigEndianU16,
        buffer_reader::BufferReader,
        mac::{data::*, MacAddr, MacFrame},
    },
    zerocopy::{AsBytes, ByteSlice, FromBytes, FromZeroes, LayoutVerified, Unaligned},
};

// RFC 1042
pub const LLC_SNAP_EXTENSION: u8 = 0xAA;
pub const LLC_SNAP_UNNUMBERED_INFO: u8 = 0x03;
pub const LLC_SNAP_OUI: [u8; 3] = [0, 0, 0];

// IEEE Std 802.2-1998, 3.2
// IETF RFC 1042
#[derive(FromZeroes, FromBytes, AsBytes, Unaligned)]
#[repr(C, packed)]
pub struct LlcHdr {
    pub dsap: u8,
    pub ssap: u8,
    pub control: u8,
    pub oui: [u8; 3],
    pub protocol_id: BigEndianU16,
}

pub struct LlcFrame<B> {
    pub hdr: LayoutVerified<B, LlcHdr>,
    pub body: B,
}

/// An LLC frame is only valid if it contains enough bytes for header AND at least 1 byte for body
impl<B: ByteSlice> LlcFrame<B> {
    pub fn parse(bytes: B) -> Option<Self> {
        let (hdr, body) = LayoutVerified::new_unaligned_from_prefix(bytes)?;
        if body.is_empty() {
            None
        } else {
            Some(Self { hdr, body })
        }
    }
}

/// A single MSDU.
pub struct Msdu<B> {
    pub dst_addr: MacAddr,
    pub src_addr: MacAddr,
    pub llc_frame: LlcFrame<B>,
}

/// An iterator to iterate over aggregated and non-aggregated MSDUs.
/// For convenience, the iterator also supports NULL data frames.
pub enum MsduIterator<B> {
    /// Iterator for a regular data frame carrying a single MSDU.
    Llc { dst_addr: MacAddr, src_addr: MacAddr, body: Option<B> },
    /// Iterator for data frames carrying aggregated MSDUs.
    Amsdu(BufferReader<B>),
    /// Iterator for NULL data frames.
    Null,
}

impl<B: ByteSlice> Iterator for MsduIterator<B> {
    type Item = Msdu<B>;
    fn next(&mut self) -> Option<Self::Item> {
        match self {
            MsduIterator::Null => None,
            MsduIterator::Llc { dst_addr, src_addr, body } => {
                let body = body.take()?;
                let llc_frame = LlcFrame::parse(body)?;
                Some(Msdu { dst_addr: *dst_addr, src_addr: *src_addr, llc_frame })
            }
            MsduIterator::Amsdu(reader) => {
                let AmsduSubframe { hdr, body } = AmsduSubframe::parse(reader)?;
                let llc_frame = LlcFrame::parse(body)?;
                Some(Msdu { dst_addr: hdr.da, src_addr: hdr.sa, llc_frame })
            }
        }
    }
}

impl<B: ByteSlice> MsduIterator<B> {
    pub fn from_data_frame_parts(
        fixed_fields: FixedDataHdrFields,
        addr4: Option<Addr4>,
        qos_ctrl: Option<QosControl>,
        body: B,
    ) -> Self {
        let fc = fixed_fields.frame_ctrl;
        if fc.data_subtype().null() {
            MsduIterator::Null
        } else if qos_ctrl.map_or(false, |x| x.amsdu_present()) {
            MsduIterator::Amsdu(BufferReader::new(body))
        } else {
            MsduIterator::Llc {
                dst_addr: data_dst_addr(&fixed_fields),
                // Safe to unwrap because data frame parsing has been successful.
                src_addr: data_src_addr(&fixed_fields, addr4).unwrap(),
                body: Some(body),
            }
        }
    }

    /// If `body_aligned` is |true| the frame's body is expected to be 4 byte aligned.
    pub fn from_raw_data_frame(data_frame: B, body_aligned: bool) -> Option<Self> {
        match MacFrame::parse(data_frame, body_aligned)? {
            MacFrame::Data { fixed_fields, addr4, qos_ctrl, body, .. } => {
                Some(Self::from_data_frame_parts(
                    *fixed_fields,
                    addr4.map(|a| *a),
                    qos_ctrl.map(|x| x.get()),
                    body,
                ))
            }
            _ => None,
        }
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::{assert_variant, test_utils::fake_frames::*},
    };

    #[test]
    fn msdu_iterator_single_llc() {
        let bytes = make_data_frame_single_llc(None, None);
        let msdus = MsduIterator::from_raw_data_frame(&bytes[..], false);
        assert!(msdus.is_some());
        let mut found_msdu = false;
        for Msdu { dst_addr, src_addr, llc_frame } in msdus.unwrap() {
            assert!(!found_msdu, "unexpected MSDU: {:x?}", llc_frame.body);
            assert_eq!(dst_addr, [3; 6]);
            assert_eq!(src_addr, [4; 6]);
            assert_eq!(llc_frame.hdr.protocol_id.to_native(), 9 << 8 | 10);
            assert_eq!(llc_frame.body, [11; 3]);
            found_msdu = true;
        }
        assert!(found_msdu);
    }

    #[test]
    fn msdu_iterator_single_llc_padding() {
        let bytes = make_data_frame_with_padding();
        let msdus = MsduIterator::from_raw_data_frame(&bytes[..], true);
        assert!(msdus.is_some());
        let mut found_msdu = false;
        for Msdu { dst_addr, src_addr, llc_frame } in msdus.unwrap() {
            assert!(!found_msdu, "unexpected MSDU: {:x?}", llc_frame.body);
            assert_eq!(dst_addr, [3; 6]);
            assert_eq!(src_addr, [4; 6]);
            assert_eq!(llc_frame.hdr.protocol_id.to_native(), 9 << 8 | 10);
            assert_eq!(llc_frame.body, [11; 5]);
            found_msdu = true;
        }
        assert!(found_msdu);
    }

    #[test]
    fn parse_llc_with_addr4_ht_ctrl() {
        let bytes = make_data_frame_single_llc(Some([1, 2, 3, 4, 5, 6]), Some([4, 3, 2, 1]));
        assert_variant!(
            MacFrame::parse(&bytes[..], false),
            Some(MacFrame::Data { body, .. }) => {
                let llc = LlcFrame::parse(body).expect("LLC frame too short");
                assert_eq!(7, llc.hdr.dsap);
                assert_eq!(7, llc.hdr.ssap);
                assert_eq!(7, llc.hdr.control);
                assert_eq!([8, 8, 8], llc.hdr.oui);
                assert_eq!([9, 10], llc.hdr.protocol_id.0);
                assert_eq!(0x090A, llc.hdr.protocol_id.to_native());
                assert_eq!(&[11, 11, 11], llc.body);
            },
            "expected data frame"
        );
    }

    #[test]
    fn parse_null_data() {
        let bytes = make_null_data_frame();
        let msdus = MsduIterator::from_raw_data_frame(&bytes[..], true);
        assert_variant!(msdus, Some(MsduIterator::Null), "expected NULL MSDU-Iterator");
        assert!(msdus.unwrap().next().is_none());
    }
}
